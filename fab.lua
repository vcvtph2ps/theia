local opt_arch = fab.option("arch", { "x86_64", "riscv64" }) or "x86_64"
local opt_build_type = fab.option("buildtype", { "debug", "release" }) or "debug"

local c = require("lang_c")
local asm = require("lang_nasm")
local linker = require("ld")

local clang = c.get_clang()
assert(clang ~= nil, "No clang compiler found")

local nasm = asm.get_nasm()
assert(nasm ~= nil, "No nasm found")

local ld = linker.get_linker("ld.lld")
assert(ld ~= nil, "No ld.lld found")

local objcopy_binary = fab.which("llvm-objcopy") or fab.which("objcopy")
assert(objcopy_binary ~= nil, "No viable objcopy found")

local python = fab.which("python3")
assert(python ~= nil, "No python3 found")

local limine_protocol = fab.dependency("limine_protocol")
local tartarus_protocol = fab.dependency("tartarus")
local uacpi = fab.dependency("uacpi")
local flanterm = fab.dependency("flanterm")
local theia_efi = fab.dependency("theia-efi")
local zstd = fab.dependency("zstd")
local libfdt = opt_arch == "riscv64" and fab.dependency("libfdt") or nil

local objcopy_rule = fab.define_rule(
    "objcopy_to_bin",
    objcopy_binary.path:gsub("@", "@@") .. " -O binary @IN@ @OUT@",
    "Objcopying @IN@ to a binary @OUT@"
)

local pad_rule = fab.define_rule(
    "postprocess_efi",
    tostring(fab.project_dir / "scripts/pad.sh") .. " @IN@ @OUT@ 4096",
    "Postprocessing @IN@ to @OUT@"
)

local zstd_gen_rule = fab.define_rule(
    "gen_zstd_decoder",
    python.path:gsub("@", "@@") .. " @IN@ " .. tostring(zstd) .. " @OUT@",
    "Generating freestanding zstd decoder @OUT@"
)

local zstd_gen_script = fab.resolve_source(fab.project_dir / "scripts/zstd/gen-decoder.py")
local zstd_decoder = zstd_gen_rule:build("zstddeclib.c", { zstd_gen_script }, {}, {
    fab.resolve_source(zstd / "lib/common/zstd_deps.h"),
    fab.resolve_source(zstd / "lib/common/xxhash.h"),
    fab.resolve_source(zstd / "build/single_file_libs/zstddeclib-in.c"),
    fab.resolve_source(zstd / "build/single_file_libs/combine.py"),
})

-- @brief Return a copy of `flags` with `extra` appended.
local function with_flags(flags, extra)
    local result = {}
    table.extend(result, flags)
    table.extend(result, extra)
    return result
end

local function source_file(relative_path)
    return fab.resolve_source(fab.project_dir / relative_path)
end

local function source_glob(pattern)
    return sources(fab.project_dir:glob(pattern))
end

local function source_glob_excluding(pattern, ...)
    return sources(fab.project_dir:glob(pattern, ...))
end

local base_flags = {
    "-std=gnu23",
    "-ffreestanding",
    "-fno-strict-aliasing",
    "-Wimplicit-fallthrough",
    "-Wmissing-field-initializers",
    "-fdiagnostics-color=always",
    "-DUACPI_BAREBONES_MODE",
    "-DUACPI_FORMATTED_LOGGING",
}

if opt_build_type == "debug" then
    table.extend(base_flags, { "-O1", "-g", "-fno-lto", "-fno-omit-frame-pointer" })
else
    table.extend(base_flags, { "-O3", "-flto" })
end

local target_flags
local kernel_arch_flags
local efi_arch_flags

if opt_arch == "x86_64" then
    target_flags = { "--target=x86_64-none-elf" }
    kernel_arch_flags = with_flags(target_flags,
        { "-mno-red-zone", "-mgeneral-regs-only", "-mabi=sysv", "-mcmodel=kernel" })
    efi_arch_flags = with_flags(target_flags, { "-mno-red-zone", "-mgeneral-regs-only", "-mabi=sysv", "-fpie" })
else
    target_flags = { "--target=riscv64-none-elf", "-march=rv64gc_zihintpause", "-mabi=lp64", "-mcmodel=medany",
        "-mno-relax", "-msmall-data-limit=8" }
    kernel_arch_flags = target_flags
    efi_arch_flags = with_flags(target_flags, { "-fpie" })
end

local warning_flags = {
    "-Wall",
    "-Wextra",
    "-Wvla",
    "-Werror",
    "-Wpedantic",
    "-Wno-language-extension-token",
    "-Wno-gnu-zero-variadic-macro-arguments",
    "-Wno-gnu-statement-expression-from-macro-expansion",
    "-Wno-error=unused-function",
    "-Wno-extra-semi",
    "-Wno-empty-translation-unit",
    "-Wmissing-prototypes",
    "-Wdocumentation",
    "-Wno-error=documentation",
    "-Wmissing-noreturn",
}

local kernel_base_flags = with_flags(base_flags, kernel_arch_flags)
local efi_base_flags = with_flags(base_flags, efi_arch_flags)

local kernel_flags = with_flags(kernel_base_flags, warning_flags)
local other_flags = with_flags(kernel_base_flags, { "-Wall", "-Wextra" })
local flanterm_flags = with_flags(other_flags, { "-DFLANTERM_FB_DISABLE_BUMP_ALLOC" })

local efi_flags = with_flags(efi_base_flags, { "-fno-stack-protector", "-DPREKERNEL_PLATFORM_EFI" })
local efi_strict_flags = with_flags(efi_flags, with_flags(warning_flags, { "-Wno-documentation" }))
local efi_dep_flags = with_flags(efi_flags, { "-Wall", "-Wextra" })
local efi_flanterm_flags = with_flags(efi_dep_flags, { "-DFLANTERM_FB_DISABLE_BUMP_ALLOC" })

if opt_build_type == "debug" then
    table.extend(kernel_flags, { "-fsanitize=undefined", "-fstack-protector-all" })
    table.extend(efi_strict_flags, { "-fsanitize=undefined" })
end

local zstd_strip_flags = { "-Os", "-g0", "-fomit-frame-pointer", "-fno-sanitize=undefined", "-fno-stack-protector" }
local zstd_kernel_flags = with_flags(kernel_base_flags, zstd_strip_flags)
local zstd_efi_flags = with_flags(efi_flags, { "-Os", "-g0", "-fomit-frame-pointer", "-fno-sanitize=undefined" })

local function include_dirs(extra)
    local result = {
        c.include_dir(fab.project_dir / ("pre_kernel/include/arch/" .. opt_arch)),
        c.include_dir(fab.project_dir / "pre_kernel/include"),
        c.include_dir(fab.project_dir / "pre_kernel/public"),
        c.include_dir(flanterm / "src"),
        c.include_dir(uacpi / "include"),
        c.include_dir(zstd / "lib"),
    }
    if libfdt ~= nil then
        table.extend(result, { c.include_dir(libfdt / "libfdt"), c.include_dir(libfdt) })
    end
    table.extend(result, extra)
    return result
end

local hosted_include_dirs = include_dirs({
    c.include_dir(limine_protocol / "include"),
    c.include_dir(tartarus_protocol),
})
local efi_include_dirs = include_dirs({ c.include_dir(theia_efi / "include") })

-- @brief Compile sources with clang, giving each object a unique prefix.
local function compile_c(flags, srcs, directories, prefix)
    local objects = {}
    for _, source in ipairs(srcs) do
        local name = prefix .. generator_artifact_name(source) .. ".o"
        table.insert(objects, clang:compile_object(name, source, directories, flags, name .. ".d"))
    end
    return objects
end

-- @brief Compile C and assembly sources, giving each object a unique prefix.
local function compile_sources(srcs, flags, directories, prefix)
    local objects = {}
    for _, source in ipairs(srcs) do
        local name = prefix .. generator_artifact_name(source) .. ".o"
        if opt_arch == "x86_64" and source.path:ends_with(".asm") then
            table.insert(objects, nasm:assemble(name, source, { "-f", "elf64", "-Werror" }, name .. ".d"))
        else
            table.insert(objects, clang:compile_object(name, source, directories, flags, name .. ".d"))
        end
    end
    return objects
end

-- Sources shared by the hosted ELF and the EFI application.
local common_sources = {}
for _, pattern in ipairs({
    "pre_kernel/src/common/**/*.c",
    "pre_kernel/src/fdt/**/*.c",
    "pre_kernel/src/loader/**/*.c",
    "pre_kernel/src/memory/**/*.c",
    "pre_kernel/src/runtime/**/*.c",
}) do
    table.extend(common_sources, source_glob(pattern))
end

table.extend(common_sources, {
    source_file("pre_kernel/src/init.c"),
    source_file("pre_kernel/src/log.c"),
    source_file("pre_kernel/src/panic.c"),
    source_file("pre_kernel/src/smp.c"),
    source_file("pre_kernel/src/spinlock.c"),
    source_file("pre_kernel/src/uacpi_kernel_api.c"),
    source_file("pre_kernel/src/boot/bootinfo.c"),
    source_file("pre_kernel/src/boot/bsp.c"),
})

local arch_shared_sources = source_glob_excluding("pre_kernel/src/arch/" .. opt_arch .. "/**/*.c",
    "!pre_kernel/src/arch/" .. opt_arch .. "/efi.c")
local arch_efi_source = source_file("pre_kernel/src/arch/" .. opt_arch .. "/efi.c")

local arch_asm_sources
if opt_arch == "x86_64" then
    arch_asm_sources = source_glob("pre_kernel/src/arch/x86_64/**/*.asm")
else
    arch_asm_sources = source_glob("pre_kernel/src/arch/riscv64/**/*.S")
end

local uacpi_sources = sources(uacpi:glob("source/*.c"))
local flanterm_sources = sources(flanterm:glob("src/*.c"))

-- hosted ELF
local hosted_sources = {}
table.extend(hosted_sources, common_sources)
table.extend(hosted_sources, arch_shared_sources)
table.extend(hosted_sources, arch_asm_sources)
table.extend(hosted_sources, uacpi_sources)
table.extend(hosted_sources, {
    source_file("pre_kernel/src/boot/boot.c"),
    source_file("pre_kernel/src/boot/limine.c"),
    source_file("pre_kernel/src/boot/tartarus.c"),
})

local hosted_objects = compile_sources(hosted_sources, kernel_flags, hosted_include_dirs, "")
table.extend(hosted_objects, compile_c(flanterm_flags, flanterm_sources, { c.include_dir(flanterm / "src") }, ""))
table.insert(hosted_objects, clang:compile_object("zstddeclib.o", zstd_decoder, {}, zstd_kernel_flags, "zstddeclib.o.d"))
table.insert(hosted_objects, source_file("kernel.o"))

local lunar = ld:link("lunar.elf", hosted_objects, {
    "-znoexecstack",
    "-e", "prekernel_entry",
}, fab.resolve_source(fab.project_dir / ("support/linker-scripts/" .. opt_arch .. ".lds")))

-- EFI application
local efi_sources = {}
table.extend(efi_sources, common_sources)
table.extend(efi_sources, arch_shared_sources)
table.extend(efi_sources, arch_asm_sources)
table.extend(efi_sources, uacpi_sources)
table.extend(efi_sources, {
    source_file("pre_kernel/src/boot/efi.c"),
    source_file("pre_kernel/src/boot/efi_boot.c"),
    source_file("pre_kernel/src/boot/gop.c"),
    source_file("pre_kernel/src/entry.c"),
    arch_efi_source,
    sources(theia_efi / ("src/entry-" .. opt_arch .. ".S"))[1],
})

local efi_objects = compile_sources(efi_sources, efi_strict_flags, efi_include_dirs, "efi_")
table.extend(efi_objects, compile_c(efi_dep_flags, sources(theia_efi / "src/relocate.c"), efi_include_dirs, "efi_"))
table.extend(efi_objects, compile_c(efi_flanterm_flags, flanterm_sources, { c.include_dir(flanterm / "src") }, "efi_"))
table.insert(efi_objects,
    clang:compile_object("efi_zstddeclib.o", zstd_decoder, {}, zstd_efi_flags, "efi_zstddeclib.o.d"))
table.insert(efi_objects, source_file("kernel.o"))

local theia_elf = ld:link("lunar.efi.elf", efi_objects, {
    "-znoexecstack",
    "-pie",
    "-zcommon-page-size=0x1000",
    "-zmax-page-size=0x1000",
}, fab.resolve_source(theia_efi / ("support/" .. opt_arch .. ".lds")))

local theia_bin = objcopy_rule:build("lunar.efi.bin", { theia_elf }, {})
local theia_firmware = pad_rule:build("lunar.efi", { theia_bin }, {})

return {
    ["lunar.elf"] = lunar,
    ["lunar.efi.elf"] = theia_elf,
    ["lunar.efi"] = theia_firmware,
}
