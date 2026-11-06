# Addon Manager (LeviLauncher native mod)

Importa addons (.mcaddon / .mcpack / .zip) y actívalos en tu mundo desde el Mod Menu,
sin salir del juego. Solo usa la API pública del preloader (ModMenu + Input), no toca
`libminecraftpe.so`.

## Cómo se usa
1. Abre el Mod Menu dentro del juego → módulo **Addon Manager**.
2. **Importar addon** → eliges el archivo → se descomprime en `behavior_packs` / `resource_packs`.
3. Elige el **mundo** (por defecto el que jugaste más recientemente).
4. Toca el interruptor del pack para activarlo/desactivarlo en ese mundo
   (escribe `world_behavior_packs.json` / `world_resource_packs.json`, con copia `.bak`).
5. Guarda, sal del mundo y vuelve a entrar: Minecraft solo carga los packs al abrir el mundo.

## Notas
- Si el pack depende de otro pack instalado (por UUID), se activa también.
- Si no encuentra `com.mojang`, pon la ruta en la config del mod (`gamePath`).
- Skin packs y world templates se ignoran.
- Compilar: sube esta carpeta a GitHub (el workflow `.github/workflows/build.yml` genera
  el `.levipack` como artifact), o usa `./scripts/package.ps1 -Abi arm64-v8a`.
