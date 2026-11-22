# Fichiers repris de Xenia (BSD, voir LICENSE)

Source : https://github.com/xenia-project/xenia, révision 95a5c3ee250f80c3b9d139658649d9ffb6db3eec.
Copiés sans modification (sauf `base/string_buffer.h` : `fmt::runtime` pour la version récente de fmt ; `base/memory.h` : `_aligned_malloc` aussi sous MinGW) : `src/xenia/gpu/{ucode,xenos,shader,shader_translator,shader_translator_disasm,registers}.*`,
`register_table.inc`, `src/xenia/base/{assert,math,platform,byte_order,memory,string_buffer,literals}.*`.
Remplacés par des cales wakfu-recomp : `base/logging.h`, `base/string.h`, `base/filesystem.h`,
`gpu/gpu_flags.h`, et `shims.cpp` (copy_and_swap_*, cvars). Usage : analyse et désassemblage du
microcode Xenos, base du traducteur de shaders du runtime.
