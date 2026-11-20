// The pipelines ZOE US draws with (ps2host::SetGpuPipelineHints, keys only - blend, write mask and the shader's
// features as the engine's renderer numbers them, no game data): made by the renderer's workers while the game
// starts, so a player's first start (or the first after a new driver or shader) does not compile them in the
// middle of play. Taken 2026-11-18 from a played session's gpu_pipelines.txt (102 keys, engine 8d640de). A key
// the engine no longer makes the same way costs one unused pipeline at the start; renew the list after renderer
// changes (copy build/mc_test/gpu_pipelines.txt after a played run).
static const u32 kPipelineKeys[] = {
    0x00000000u, 0x00000083u, 0x00000085u, 0x00000089u, 0x00000091u, 0x000000C9u, 0x00000E00u, 0x00001000u,
    0x00001089u, 0x00001091u, 0x000010B1u, 0x0E261000u, 0x0E261089u, 0x0E261091u, 0x0E361000u, 0x0E361089u,
    0x0E361091u, 0x0E661000u, 0x0E661089u, 0x0E661091u, 0x0E761000u, 0x0E761089u, 0x0E761091u, 0x0E960089u,
    0x0E960091u, 0x0E9600C9u, 0x0EA60091u, 0x0EA61000u, 0x0EA61089u, 0x0EA610B1u, 0x0EAE0089u, 0x0EAE0091u,
    0x0EB60000u, 0x0EB60085u, 0x0EB60089u, 0x0EB60091u, 0x0EB61000u, 0x0EB61089u, 0x0EB61091u, 0x0EB610B1u,
    0x0EBE0000u, 0x0EBE0089u, 0x0EBE0091u, 0x0EE60085u, 0x0EE60089u, 0x0EE60091u, 0x0EE61000u, 0x0EE61089u,
    0x0EE61091u, 0x0EE610B1u, 0x0EEE0089u, 0x0EEE0091u, 0x0EF60000u, 0x0EF60085u, 0x0EF60089u, 0x0EF60091u,
    0x0EF600C9u, 0x0EF61000u, 0x0EF61089u, 0x0EF61091u, 0x0EF610B1u, 0x0EFE0000u, 0x0EFE0089u, 0x0EFE0091u,
    0x0EFE1089u, 0x0EFF0089u, 0x0EFF0091u, 0x0EFF1089u, 0x0F260000u, 0x0F261091u, 0x0F360000u, 0x0F361089u,
    0x0F361091u, 0x0F660000u, 0x0F661089u, 0x0F661091u, 0x0F760000u, 0x0F761000u, 0x0F761089u, 0x0F761091u,
    0x0FA60083u, 0x0FA60089u, 0x0FA61089u, 0x0FA61091u, 0x0FB60083u, 0x0FB60089u, 0x0FB61089u, 0x0FB61091u,
    0x0FE60083u, 0x0FE60089u, 0x0FE61089u, 0x0FE61091u, 0x0FF60000u, 0x0FF60083u, 0x0FF60089u, 0x0FF61089u,
    0x0FF61091u, 0x0FFF0000u, 0x0FFF0E00u, 0x0FFF1000u, 0x0FFF1089u, 0x0FFF1091u,
};
