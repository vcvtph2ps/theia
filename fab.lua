local opt_arch = fab.option("arch", { "x86_64", "riscv64" }) or "x86_64"
local opt_build_type = fab.option("buildtype", { "debug", "release" }) or "debug"

local c = require("lang_c")
local asm = require("lang_nasm")
local linker = require("ld")

local project_dir = fab.project_dir

local clang = c.get_clang()
assert(clang ~= nil, "No clang compiler found")

local nasm = asm.get_nasm()
assert(nasm ~= nil, "No nasm found")

local ld = linker.get_linker("ld.lld")
assert(ld ~= nil, "No ld.lld found")

local objcopy_binary = fab.which("llvm-objcopy") or fab.which("objcopy")
assert(objcopy_binary ~= nil, "No viable objcopy found")

local objcopy_path = objcopy_binary.path:gsub("@", "@@")

local objcopy_rule = fab.define_rule(
    "objcopy_to_bin",
    objcopy_path .. " -O binary @IN@ @OUT@",
    "Objcopying @IN@ to a binary @OUT@"
)

local pad_rule = fab.define_rule(
    "postprocess_efi",
    tostring(project_dir / "scripts/pad.sh") .. " @IN@ @OUT@ 4096",
    "Postprocessing @IN@ to @OUT@"
)

local limine_protocol = fab.dependency("limine_protocol")
local tartarus_protocol = fab.dependency("tartarus")
local uacpi = fab.dependency("uacpi")
local flanterm = fab.dependency("flanterm")
local theia_efi = fab.dependency("theia-efi")
local zstd = fab.dependency("zstd")

local python = fab.which("python3")
assert(python ~= nil, "No python3 found")

local zstd_gen_rule = fab.define_rule(
    "gen_zstd_decoder",
    python.path:gsub("@", "@@") .. " @IN@ " .. tostring(zstd) .. " @OUT@",
    "Generating freestanding zstd decoder @OUT@"
)
local zstd_gen_script = fab.resolve_source(project_dir / "scripts/zstd/gen-decoder.py")
local zstd_decoder = zstd_gen_rule:build("zstddeclib.c", { zstd_gen_script }, {}, {
    fab.resolve_source(zstd / "lib/common/zstd_deps.h"),
    fab.resolve_source(zstd / "lib/common/xxhash.h"),
    fab.resolve_source(zstd / "build/single_file_libs/zstddeclib-in.c"),
    fab.resolve_source(zstd / "build/single_file_libs/combine.py"),
})

local base_c_flags = {
    "-std=gnu23",
    "-ffreestanding",

    "-fno-strict-aliasing",
    "-Wimplicit-fallthrough",
    "-Wmissing-field-initializers",

    "-fdiagnostics-color=always",
    "-DUACPI_BAREBONES_MODE",
    "-DUACPI_FORMATTED_LOGGING",
}

if opt_build_type == "release" then
    table.extend(base_c_flags, {
        "-O3",
        "-flto",
    })
elseif opt_build_type == "debug" then
    table.extend(base_c_flags, {
        "-O1",
        "-g",
        "-fno-lto",
        "-fno-omit-frame-pointer",
    })
end

-- @brief Return a copy of `flags` with `extra` appended.
local function with_flags(flags, extra)
    local out = {}
    table.extend(out, flags)
    table.extend(out, extra)
    return out
end

local arch_target_flags
local arch_kernel_flags
local arch_efi_flags

if opt_arch == "x86_64" then
    arch_target_flags = { "--target=x86_64-none-elf" }
    arch_kernel_flags = with_flags(arch_target_flags, {
        "-mno-red-zone",
        "-mgeneral-regs-only",
        "-mabi=sysv",
        "-mcmodel=kernel",
    })
    arch_efi_flags = with_flags(arch_target_flags, {
        "-mno-red-zone",
        "-mgeneral-regs-only",
        "-mabi=sysv",
        "-fpie",
    })
elseif opt_arch == "riscv64" then
    arch_target_flags = { "--target=riscv64-none-elf" }
    local riscv_flags = {
        "-march=rv64gc_zihintpause",
        "-mabi=lp64",
        "-mcmodel=medany",
        "-mno-relax",
        "-msmall-data-limit=8",
    }
    arch_kernel_flags = with_flags(arch_target_flags, riscv_flags)
    arch_efi_flags = with_flags(with_flags(arch_target_flags, riscv_flags), { "-fpie" })
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

local c_flags = with_flags(base_c_flags, arch_kernel_flags)
local kernel_flags = with_flags(c_flags, warning_flags)
local other_flags = with_flags(c_flags, { "-Wall", "-Wextra" })
if opt_build_type == "debug" then
    table.extend(kernel_flags, {
        "-fsanitize=undefined",
        "-fstack-protector-all",
    })
end

local efi_flags = with_flags(base_c_flags, arch_efi_flags)
table.extend(efi_flags, { "-fno-stack-protector", "-DPREKERNEL_PLATFORM_EFI" })
local efi_strict_flags = with_flags(efi_flags, warning_flags)

table.extend(efi_strict_flags, { "-Wno-documentation" })
local efi_dep_flags = with_flags(efi_flags, { "-Wall", "-Wextra" })
local flanterm_flags = with_flags(other_flags, { "-DFLANTERM_FB_DISABLE_BUMP_ALLOC" })
local efi_flanterm_flags = with_flags(efi_dep_flags, { "-DFLANTERM_FB_DISABLE_BUMP_ALLOC" })
if opt_build_type == "debug" then
    table.extend(efi_strict_flags, { "-fsanitize=undefined" })
end

local zstd_legacy_flags = with_flags(c_flags, {
    "-Os",
    "-g0",
    "-fomit-frame-pointer",
    "-fno-sanitize=undefined",
    "-fno-stack-protector",
})
local zstd_efi_flags = with_flags(efi_flags, {
    "-Os",
    "-g0",
    "-fomit-frame-pointer",
    "-fno-sanitize=undefined",
})

-- @brief Compile a list of sources with clang, giving each object a unique prefix.
local function compile_c(flags, srcs, include_dirs, prefix)
    local objects = {}
    for _, source in ipairs(srcs) do
        local name = prefix .. generator_artifact_name(source) .. ".o"
        table.insert(objects, clang:compile_object(name, source, include_dirs, flags, name .. ".d"))
    end
    return objects
end

-- @brief Assemble a list of nasm sources, giving each object a unique prefix.
local function assemble_nasm(srcs, prefix)
    local objects = {}
    for _, source in ipairs(srcs) do
        local name = prefix .. generator_artifact_name(source) .. ".o"
        table.insert(objects, nasm:assemble(name, source, { "-f", "elf64", "-Werror" }, name .. ".d"))
    end
    return objects
end

local function get_prekernel_objs(flags, include_dirs, zstd_flags)
    local pre_kernel_sources = sources(project_dir:glob("pre_kernel/src/**/*.c", "!pre_kernel/src/arch/**",
        "!pre_kernel/src/entry.c", "!pre_kernel/src/boot/efi.c", "!pre_kernel/src/boot/efi_boot.c",
        "!pre_kernel/src/boot/gop.c"))
    table.extend(pre_kernel_sources,
        sources(project_dir:glob("pre_kernel/src/arch/" .. opt_arch .. "/**/*.c",
            "!pre_kernel/src/arch/" .. opt_arch .. "/efi.c")))
    table.extend(pre_kernel_sources, sources(uacpi:glob("source/*.c")))

    if opt_arch == "x86_64" then
        table.extend(pre_kernel_sources,
            sources(project_dir:glob("pre_kernel/src/arch/x86_64/**/*.asm", "!pre_kernel/src/arch/x86_64/efi_handoff.asm")))
    elseif opt_arch == "riscv64" then
        table.extend(pre_kernel_sources, sources(project_dir:glob("pre_kernel/src/arch/riscv64/**/*.S")))
    end

    local generators = {
        c = function(srcs) return clang:generate(srcs, flags, include_dirs) end
    }

    if opt_arch == "x86_64" then
        local nasm_flags = { "-f", "elf64", "-Werror" }
        generators.asm = function(srcs) return nasm:generate(srcs, nasm_flags) end
    elseif opt_arch == "riscv64" then
        generators.S = function(srcs) return clang:generate(srcs, flags, include_dirs) end
    end

    local objects = generate(pre_kernel_sources, generators)
    table.insert(objects,
        clang:compile_object("zstddeclib.o", zstd_decoder, {}, zstd_flags, "zstddeclib.o.d"))
    return objects
end

local legacy_include_dirs = {
    c.include_dir(project_dir / ("pre_kernel/include/arch/" .. opt_arch)),
    c.include_dir(project_dir / "pre_kernel/include"),
    c.include_dir(project_dir / "pre_kernel/public"),
    c.include_dir(limine_protocol / "include"),
    c.include_dir(tartarus_protocol),
    c.include_dir(flanterm / "src"),
    c.include_dir(uacpi / "include"),
    c.include_dir(zstd / "lib"),
}

if opt_arch == "riscv64" then
    local libfdt = fab.dependency("libfdt")

    -- we need to include libfdt c files to override libfdt_env.h :/
    table.insert(legacy_include_dirs, c.include_dir(libfdt / "libfdt"))
    table.insert(legacy_include_dirs, c.include_dir(libfdt))
end

local legacy_objects = {}
local flanterm_objects = compile_c(flanterm_flags, sources(flanterm:glob("src/*.c")), { c.include_dir(flanterm / "src") },
    "")
table.extend(legacy_objects, flanterm_objects)
table.extend(legacy_objects, get_prekernel_objs(kernel_flags, legacy_include_dirs, zstd_legacy_flags))
table.extend(legacy_objects, { fab.resolve_source(project_dir / "kernel.o") })

local linker_script = fab.resolve_source(project_dir / ("support/linker-scripts/" .. opt_arch .. ".lds"))

local lunar = ld:link("lunar.elf", legacy_objects, {
    "-znoexecstack",
    "-e", "prekernel_entry"
}, linker_script)

local efi_include_dirs = {
    c.include_dir(project_dir / ("pre_kernel/include/arch/" .. opt_arch)),
    c.include_dir(project_dir / "pre_kernel/include"),
    c.include_dir(project_dir / "pre_kernel/public"),
    c.include_dir(flanterm / "src"),
    c.include_dir(uacpi / "include"),
    c.include_dir(theia_efi / "include"),
    c.include_dir(zstd / "lib"),
}

if opt_arch == "riscv64" then
    local libfdt = fab.dependency("libfdt")

    table.insert(efi_include_dirs, c.include_dir(libfdt / "libfdt"))
    table.insert(efi_include_dirs, c.include_dir(libfdt))
end


local efi_common_sources = sources(project_dir:glob("pre_kernel/src/**/*.c",
    "!pre_kernel/src/arch/**",
    "!pre_kernel/src/boot/limine.c",
    "!pre_kernel/src/boot/tartarus.c"))

table.extend(efi_common_sources, sources(project_dir:glob("pre_kernel/src/arch/" .. opt_arch .. "/**/*.c")))
table.extend(efi_common_sources, sources(uacpi:glob("source/*.c")))

local efi_asm_sources
local efi_S_sources = sources(theia_efi / ("src/entry-" .. opt_arch .. ".S"))

if opt_arch == "x86_64" then
    efi_asm_sources = sources(project_dir:glob("pre_kernel/src/arch/x86_64/**/*.asm"))
elseif opt_arch == "riscv64" then
    table.extend(efi_S_sources, sources(project_dir:glob("pre_kernel/src/arch/riscv64/**/*.S")))
end

local efi_objects = {}
table.extend(efi_objects, compile_c(efi_strict_flags, efi_common_sources, efi_include_dirs, "efi_"))
table.extend(efi_objects, compile_c(efi_strict_flags, efi_S_sources, efi_include_dirs, "efi_"))
table.extend(efi_objects, compile_c(efi_dep_flags, sources(theia_efi / "src/relocate.c"), efi_include_dirs, "efi_"))
table.extend(efi_objects,
    compile_c(efi_flanterm_flags, sources(flanterm:glob("src/*.c")), { c.include_dir(flanterm / "src") }, "efi_"))
table.insert(efi_objects,
    clang:compile_object("efi_zstddeclib.o", zstd_decoder, {}, zstd_efi_flags, "efi_zstddeclib.o.d"))
if efi_asm_sources ~= nil then
    table.extend(efi_objects, assemble_nasm(efi_asm_sources, "efi_"))
end
table.extend(efi_objects, { fab.resolve_source(project_dir / "kernel.o") })

local efi_linker_script = fab.resolve_source(theia_efi / ("support/" .. opt_arch .. ".lds"))

local theia_elf = ld:link("lunar.efi.elf", efi_objects, {
    "-znoexecstack",
    "-pie",
    "-zcommon-page-size=0x1000",
    "-zmax-page-size=0x1000"
}, efi_linker_script)

local theia_bin = objcopy_rule:build("lunar.efi.bin", { theia_elf }, {})
local theia_efi = pad_rule:build("lunar.efi", { theia_bin }, {})

return {
    ["lunar.elf"] = lunar,
    ["lunar.efi.elf"] = theia_elf,
    ["lunar.efi"] = theia_efi,
}
