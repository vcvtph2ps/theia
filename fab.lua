local opt_arch = fab.option("arch", { "x86_64", "riscv64" }) or "x86_64"
local opt_bootloader = fab.option("bootloader", { "limine", "tartarus" }) or "tartarus"
local opt_build_type = fab.option("buildtype", { "debug", "release" }) or "debug"

if opt_bootloader == "tartarus" and opt_arch == "riscv64" then
    error("Tartarus does not support riscv64")
end

local c = require("lang_c")
local asm = require("lang_nasm")
local linker = require("ld")

local clang = c.get_clang()
assert(clang ~= nil, "No clang compiler found")

local nasm = asm.get_nasm()
assert(nasm ~= nil, "No nasm found")

local ld = linker.get_linker("ld.lld")
assert(ld ~= nil, "No ld.lld found")

local limine_protocol = fab.git(
    "limine_protocol",
    "https://github.com/Limine-Bootloader/limine-protocol.git",
    "5b9d13e"
)

local tartarus_protocol = fab.git(
    "tartarus",
    "https://github.com/elysium-os/tartarus-bootloader.git",
    "e95c3651f16d2560424c2d0d514aa8467d440053"
)

local uacpi = fab.git(
    "uacpi",
    "https://github.com/uACPI/uACPI.git",
    "9c9b26d6291a1cdd9014cc5bb6b03e596697cbfd"
)

local libfdt = nil
if opt_arch == "riscv64" then
    libfdt = fab.git(
        "dtc",
        "https://github.com/dgibson/dtc.git",
        "v1.7.0"
    )
end

local flanterm = fab.git(
    "flanterm",
    "https://github.com/Mintsuki/Flanterm.git",
    "e231edd0e508022521c8233392aabc8ce5c9b242"
)

local function get_prekernel_objs(kernel_flags)
    local pre_kernel_sources = sources(fab.glob("pre_kernel/src/**/*.c", "!pre_kernel/src/arch/**"))
    table.extend(pre_kernel_sources, sources(fab.glob(path("pre_kernel/src/arch", opt_arch, "**/*.c"))))
    table.extend(pre_kernel_sources, sources(fab.glob("source/*.c", { relative_to = uacpi.path })))

    if opt_arch == "x86_64" then
        table.extend(pre_kernel_sources, sources(fab.glob("pre_kernel/src/arch/x86_64/**/*.asm")))
    elseif opt_arch == "riscv64" then
        table.extend(pre_kernel_sources, sources(fab.glob("pre_kernel/src/arch/riscv64/**/*.S")))
    end

    local pre_kernel_include_dirs = {
        c.include_dir(path("pre_kernel/include/arch/", opt_arch)),
        c.include_dir("pre_kernel/include"),
        c.include_dir("pre_kernel/public")
    }

    table.insert(pre_kernel_include_dirs, c.include_dir(path(fab.build_dir(), limine_protocol.path, "include")))
    table.insert(pre_kernel_include_dirs, c.include_dir(path(fab.build_dir(), tartarus_protocol.path)))
    table.insert(pre_kernel_include_dirs, c.include_dir(path(fab.build_dir(), flanterm.path, "src")))

    if libfdt ~= nil then
        -- we need to include libfdt c files to override libfdt_env.h :/
        table.insert(pre_kernel_include_dirs, c.include_dir(path(fab.build_dir(), libfdt.path, "libfdt")))
        table.insert(pre_kernel_include_dirs, c.include_dir(path(fab.build_dir(), libfdt.path)))
    end

    table.insert(pre_kernel_include_dirs, c.include_dir(path(fab.build_dir(), uacpi.path, "include")))

    local generators = {
        c = function(srcs) return clang:generate(srcs, kernel_flags, pre_kernel_include_dirs) end
    }

    if opt_arch == "x86_64" then
        local nasm_flags = { "-f", "elf64", "-Werror" }
        generators.asm = function(srcs) return nasm:generate(srcs, nasm_flags) end
    elseif opt_arch == "riscv64" then
        generators.S = function(srcs) return clang:generate(srcs, kernel_flags, pre_kernel_include_dirs) end
    end

    return generate(pre_kernel_sources, generators)
end


local c_flags = {
    "-std=gnu23",
    "-ffreestanding",

    "-fno-strict-aliasing",
    "-Wimplicit-fallthrough",
    "-Wmissing-field-initializers",

    "-fdiagnostics-color=always",
    "-DUACPI_BAREBONES_MODE",
    "-DUACPI_FORMATTED_LOGGING",
}

-- Flags
if opt_build_type == "release" then
    table.extend(c_flags, {
        "-O3",
        "-flto",
    })
elseif opt_build_type == "debug" then
    table.extend(c_flags, {
        "-O0",
        "-g",
        "-fno-lto",
        "-fno-omit-frame-pointer",
    })
end


if opt_arch == "x86_64" then
    table.extend(c_flags, {
        "--target=x86_64-none-elf",
        "-mno-red-zone",
        "-mgeneral-regs-only",
        "-mabi=sysv",
        "-mcmodel=kernel",
        "-D__ARCH_X86_64__"
    })
elseif opt_arch == "riscv64" then
    table.extend(c_flags, {
        "--target=riscv64-none-elf",
        "-march=rv64gc_zihintpause",
        "-mabi=lp64",
        "-mcmodel=medany",
        "-mno-relax",
        "-msmall-data-limit=8",
        "-D__ARCH_RISCV64__"
    })
end

if opt_bootloader == "limine" then
    table.insert(c_flags, "-D__BOOTLOADER_LIMINE__")
elseif opt_bootloader == "tartarus" then
    table.insert(c_flags, "-D__BOOTLOADER_TARTARUS__")
end

local objects = {}
local other_flags = {}
table.extend(other_flags, c_flags)
table.extend(other_flags, {
    "-Wall",
    "-Wextra",
})

local flanterm_sources = {}
table.extend(flanterm_sources, sources(fab.glob("src/*.c", { relative_to = flanterm.path })))
local flanterm_objects = generate(flanterm_sources, {
    c = function(sources)
        return clang:generate(sources, c_flags,
            c.include_dir(path(fab.build_dir(), flanterm.path, "src")))
    end
})

table.extend(objects, flanterm_objects)
local kernel_flags = {}
table.extend(kernel_flags, c_flags)
table.extend(kernel_flags, {
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
    "-Wno-error=unused-function",
    "-Wmissing-prototypes",
    "-Wdocumentation",
    "-Wmissing-noreturn",
})

local linker_script = fab.def_source("support/" .. opt_arch .. ".lds")

if opt_build_type == "debug" then
    table.extend(kernel_flags, {
        "-fsanitize=undefined",
        "-fstack-protector-all"
    })
end

local prekernel_objs = get_prekernel_objs(kernel_flags)

table.extend(objects, prekernel_objs)
table.extend(objects, { fab.def_source("kernel.o") })

local lunar = ld:link("lunar.elf", objects, {
    "-znoexecstack",
    "-e", "prekernel_entry_" .. opt_bootloader
}, linker_script)

return {
    install = {
        ["lunar.elf"] = lunar
    }
}
