# Super Monkey Ball Jr. (GBA) — Wear OS / Android / Desktop / Web — Plano

ROM: `Super Monkey Ball Jr. (USA).gba` · código **ALUE** · maker 78 (THQ) · dev Realism · 4 MiB (cheia)
SHA1 `0d994a58b7acdfe9357e5405acebe232128b80fe` · save **EEPROM_V122** · ~2.000 funções Thumb + poucas rotinas ARM (provavelmente copiadas p/ IWRAM)
Indícios de rasterizador 3D por software em bitmap (literais `0x0600A000` = back-buffer mode 4/5). Confirmar no mGBA (DISPCNT).

## Estado da arte (pesquisa nov/2026)
- **Não existe** decomp, recomp nem port do SMB Jr GBA. Decomps "Monkey Ball" existentes são do GameCube.
- **mstan/gbarecomp**: recompilador estático GBA→C++ com runtime (PPU HLE scanline, APU, DMA, IRQ, saves), fallback interpretador/JIT, alvos Windows/Android/Web (SDL2). Base dos MinishCapRecomp, EmeraldRecomp etc. **Licença PolyForm Noncommercial**. SMB Jr não está na lista.
- **Cores embutíveis**: mGBA (MPL-2.0, preciso, base do WearGBA), gpSP (GPLv2, dynarec ARM, mais leve), SkyEmu (MIT, web).
- **WearGBA** (mGBA) já mostra que GBA roda bem em relógio com layouts redondos.
- Hi-res / widescreen só é possível substituindo o rasterizador do jogo (HLE) dentro de uma recomp.

## Arquitetura
```
core/            libmgba (fase 1)  →  gbarecomp runtime + código gerado (fase 2)
platform/        SDL2/SDL3: vídeo, áudio, gamepad, sensores (reaproveitar ctr-wearos / mgs-wearos)
input/           camada única: touch-trail (gen1recomp), gyro (CTR), gamepad, teclado
                 → saída analógica (x,y) → PWM no D-pad (fase 1) / injeção analógica (fase 2)
android/ wear/   Gradle (copiar de ctr-wearos/android), NativeActivity/SDLActivity
web/             Emscripten + DeviceOrientation API + Gamepad API
```

### Controles (todas as plataformas)
| Modo | Como funciona |
|---|---|
| Swipe (padrão no relógio) | âncora segue o dedo (estilo gen1recomp); distância = intensidade |
| Giroscópio | inclinação do pulso/celular = inclinação do mundo; calibrar "zero" com toque longo; deadzone + curva |
| Híbrido | gyro inclina, toque só para A/B/Start (touch guard como no CTR) |
| Gamepad | analógico + D-pad (SDL GameController; Bluetooth no relógio/celular) |
| Coroa giratória | câmera / menus |

O GBA só tem D-pad digital: na fase 1 o analógico vira **PWM** (D-pad pulsado com duty-cycle proporcional à inclinação), o que dá controle fino sem mexer no jogo. Na fase 2, patch da rotina de leitura de input para receber valor analógico real.

### Tela redonda
240×160 (3:2): modos **Fit** (inscrito), **Fill** (zoom/corte, HUD reposicionado depois) e **Zoom** (coroa). Vinheta circular e botões na borda.

## Fases, horas e tokens (estimativa, trabalho de agente + seus testes)

| Fase | Entrega | Horas | Tokens |
|---|---|---|---|
| 0. Prova de vida | libmgba + SDL no desktop, ROM rodando, save EEPROM, gamepad | 3–5 h | 1–2 M |
| 1. Wear OS MVP | APK do relógio: tela redonda, swipe, gyro, PWM, touch guard, coroa | 8–12 h | 4–7 M |
| 1b. Android phone + Web | mesmo core; gyro do celular; build Emscripten com DeviceOrientation | 4–6 h | 2–3 M |
| **Subtotal "versão funcionando"** | **4 plataformas jogáveis** | **15–23 h** | **7–12 M** |
| 2. Recomp estática | alvo novo no gbarecomp (descoberta de funções, overlays IWRAM, saves), 100% nativo | 30–60 h | 15–35 M |
| 3. Input analógico nativo | patch da rotina de input (requer fase 2 ou hook no mGBA) | 6–12 h | 3–6 M |
| 4. Render HLE | reverter rasterizador → hi-res / widescreen / 60 fps | 40–100 h+ | 25–60 M+ |

## Riscos
- **Licença**: gbarecomp é não comercial. mGBA (MPL) é seguro. O jogo e a ROM nunca são distribuídos: o usuário importa a própria ROM.
- **Desempenho**: o rasterizador é pesado no GBA real, mas o mGBA emula 16 MHz com folga em A53/A55. Fallback: gpSP.
- **Fase 2**: rotinas ARM em IWRAM e ponteiros de função exigem tabelas manuais; o fallback JIT do gbarecomp reduz o risco.
- **Segurança**: a página do TCRF sobre o jogo continha prompt injection dirigido a agentes. Não usar como fonte automatizada.

## Próximo passo
Fase 0: clonar mGBA (tag estável), criar `smbjr-wearos/` com CMake + SDL, rodar a ROM no desktop e confirmar o modo de vídeo e o formato de save.
