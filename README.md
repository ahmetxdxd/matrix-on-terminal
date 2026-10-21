# ⚡ The Matrix: Rooftop Bullet-Time in 3D Terminal & GPU

> **"No one has ever done anything like this."**  
> **"That's why it's going to work."**

A real-time, interactive 3D ray-marched recreation of the legendary **Matrix rooftop bullet-dodge scene**, rendered directly inside your terminal with pure ANSI characters and hardware GPU acceleration:

- **Limbo Bullet-Dodge:** Neo bends backwards at 65° with analytic Inverse Kinematics (IK) legs, dynamic coat physics, and sleek sunglasses.
- **3 Armed Agents:** In dark suits, ties, and shades, firing pistols with dynamic muzzle flashes.
- **Bullet Time & Shockwaves:** 18 bullets with glowing brass cores and aerodynamic vapor shockwave trails cutting through the air.
- **Wet Rooftop Scene:** Water tower, stairwell with warm tungsten lamp, AC unit, pipes, crates, skylight, and puddles with ripple rings and skyline reflections.
- **Infinite Procedural Skyline:** Metropolis with lit skyscraper windows and blinking red communication antenna beacons.
- **Rain Toggle (<kbd>G</kbd>):** Turn off rain anytime for a razor-sharp, crystal-clear view.
- **2x Ultra Super-Sampling SSAA (<kbd>+</kbd> / <kbd>T</kbd>):** Boost resolution and eliminate aliasing.
- **GPU Acceleration (CPU Relief):** Offscreen OpenGL raymarching computes the millions of distance-field steps in hardware and feeds the framebuffer back to the terminal, delivering buttery-smooth 60–120+ FPS with virtually zero CPU strain!
---

## 🌟 Visual Showcase (Live Terminal Recordings)

Both visual modes are captured directly from real terminal sessions:

| 🟢 Green Matrix Code ASCII Edition | 🎬 24-Bit TrueColor Cinematic Edition |
| :---: | :---: |
| <img src="assets/matrix_code_terminal.gif" width="100%" alt="Green Matrix Code Terminal Recording"> | <img src="assets/matrix_truecolor_terminal.gif" width="100%" alt="Truecolor Terminal Recording"> |
| *Cascading digital rain streams, high-contrast character silhouettes, and blazing white-gold bullet shockwaves.* | *Wet tar reflections, puddle ripples, soft shadows, ambient occlusion, rim light, and cinematic film tone-mapping.* |

---

## 🚀 The 4 Editions

The repository provides exactly 4 clean, dedicated implementations:

| File | Target Binary | Renderer Mode | Hardware | Terminal Experience |
| :--- | :--- | :--- | :--- | :--- |
| **`main.c`** | `main` | **24-Bit TrueColor** | CPU (OpenMP) | Cinematic subpixel half-blocks (`▀`), ACES tonemapping, reflections, and puddle ripple physics. |
| **`ascii.c`** | `ascii` | **Green Matrix Code** | CPU (OpenMP) | Falling digital rain streams, white-hot stream heads, high-contrast character silhouettes. |
| **`gpu-main.c`** | `gpu-main` | **24-Bit TrueColor** | **GPU-Accelerated** | Offscreen OpenGL raymarching feeds the terminal half-blocks, running at **60–120+ FPS** without CPU lag. |
| **`gpu-ascii.c`** | `gpu-ascii` | **Green Matrix Code** | **GPU-Accelerated** | Offscreen OpenGL raymarching feeds the terminal Matrix rain glyphs, silky smooth with 0% CPU strain. |

---

## 🕹️ Interactive Controls

Press <kbd>H</kbd> inside any of the programs to open the interactive in-terminal help overlay.

| Key | Action |
| :--- | :--- |
| <kbd>W</kbd> / <kbd>S</kbd> | Move Forward / Backward |
| <kbd>A</kbd> / <kbd>D</kbd> | Strafe Left / Right |
| <kbd>R</kbd> / <kbd>F</kbd> | Fly Up / Down |
| <kbd>↑</kbd> <kbd>↓</kbd> <kbd>←</kbd> <kbd>→</kbd> or <kbd>I</kbd> <kbd>K</kbd> <kbd>J</kbd> <kbd>L</kbd> | Look Around (Yaw / Pitch) |
| <kbd>C</kbd> | Toggle 360° Cinematic Orbit Camera |
| <kbd>G</kbd> | **Toggle Rain ON / OFF** (Disables code rain / puddle ripples) |
| <kbd>+</kbd> / <kbd>=</kbd> | **Increase Resolution / Enable 2x Ultra Super-Sampling (SSAA)** |
| <kbd>-</kbd> / <kbd>_</kbd> | **Decrease Resolution Scale** |
| <kbd>T</kbd> | **Toggle 2x Ultra Super-Sampling SSAA** |
| <kbd>B</kbd> | Toggle Bullet Time (Slow Motion vs. Normal Speed) |
| <kbd>[</kbd> / <kbd>]</kbd> | Decrease / Increase Time Speed |
| <kbd>P</kbd> or <kbd>Space</kbd> | Pause / Resume World Simulation |
| <kbd>Z</kbd> / <kbd>U</kbd> | Zoom In / Out (Adjust Field of View) |
| <kbd>1</kbd>–<kbd>4</kbd> | Set Fixed Resolution Divisor |
| <kbd>H</kbd> | Toggle Help HUD Overlay |
| <kbd>X</kbd> | Clean Exit (Restores Terminal) |

---

## ⚡ Quick Start & Compilation

### Requirements
- GCC or Clang with OpenMP support.
- For GPU editions: GLFW and OpenGL (`libglfw3-dev`, `libgl1-mesa-dev` on Debian/Ubuntu, `glfw` and `mesa` on Arch).
- Any modern 24-bit TrueColor terminal emulator (e.g. Alacritty, Kitty, WezTerm, Ghostty, Foot, iTerm2, Windows Terminal, GNOME Terminal).

### Build All 4 Targets

```bash
# Clone the repository
git clone https://github.com/USERNAME/matrix-on-terminal.git
cd matrix-on-terminal

# Compile all 4 editions simultaneously
make all
```

### Run

```bash
# 1. CPU TrueColor Cinematic Terminal Edition
./main

# 2. CPU Green Matrix Code ASCII Edition
./ascii

# 3. GPU-Accelerated TrueColor Terminal Edition (High FPS, Low CPU)
./gpu-main

# 4. GPU-Accelerated Green Matrix Code ASCII Edition (High FPS, Low CPU)
./gpu-ascii
```

> **Tip:** Maximize your terminal window and set font size between 9pt and 11pt for the sharpest 3D visual density.

---

## 📐 Technical Architecture

```
                       [ Simulation Clock (dt) ]
                                  |
                +-----------------+-----------------+
                |                                   |
         [ Neo IK & Limbo ]                 [ Bullet Physics ]
                |                                   |
                +-----------------+-----------------+
                                  |
                      [ Camera (Orbit / 6-DOF) ]
                                  |
                +-----------------+-----------------+
                |                                   |
    [ CPU Raymarcher: OpenMP ]             [ GPU Raymarcher: OpenGL/GLSL ]
       (main.c / ascii.c)                   (gpu-main.c / gpu-ascii.c)
                |                                   |
                |                          [ glReadPixels (~1.8ms) ]
                |                                   |
                +-----------------+-----------------+
                                  |
                         [ Shading & Stride ]
                                  |
                +-----------------+-----------------+
                |                                   |
      [ TrueColor Mode ]                   [ Matrix Code Mode ]
    (ANSI half-blocks '▀')               (Cascading rain streams,
    (ACES tonemapping)                    Phosphor green glyphs,
    (Wet tar reflections)                 White-hot stream heads)
                |                                   |
                +-----------------+-----------------+
                                  |
                       [ ANSI Screen Buffer ]
                       (Double-buffered I/O)
                                  |
                          [ Terminal View ]
```

### 1. GPU Raymarching with Terminal Readback
The GPU editions (`gpu-main.c` and `gpu-ascii.c`) execute the complete signed-distance-field (SDF) raymarching equation, soft shadow march, normal calculations, and material shading directly on the GPU shader cores via an offscreen framebuffer. The pixel buffer is read back in ~1.8 ms and translated into ANSI terminal sequences on the CPU. This frees 100% of the raymarching computation from the CPU, allowing the terminal to update at 60–120+ FPS even on laptops or multi-monitor setups.

### 2. High-Contrast Matrix "Code Vision"
Unlike naive ASCII conversion which renders dark areas as unreadable black voids, the Matrix code engine classifies materials explicitly:
- **Characters:** Highlighted with bright phosphor green and sharp Fresnel rim-lighting.
- **Bullets:** Rendered with glowing golden and white cores (`@`, `#`, `*`) and aerodynamic vapor rings.
- **Rooftop & City:** Shaded with contour lines and neon-lit windows.
- **Sky:** Rendered in pure deep black, creating razor-sharp contrast for falling digital rain streams.

---
## 📜 License

MIT License. Free for personal, educational, and creative use.
