## Setup
1. Download the ZIP and extract it.
2. Move the extracted folder into your Unreal project's `Plugins` folder
   (create the folder if it doesn't exist).
3. Launch the project. If prompted to rebuild the plugin modules, choose Yes.
4. Look through a **Cine Camera Actor** — the simulation only affects
   CineCameraActor views, not standard viewports.

Requires a C++ Unreal project (Unreal Engine 5.7).

## Console commands

**Core**
- `r.ALEV4DGA.Enabled 0|1` — toggle the simulation (default 1)
- `r.ALEV4DGA.LogC4 0|1` — apply the ARRI LogC4 encode (default 1)

**Dual Gain Architecture**
- `r.ALEV4DGA.HighGain <float>` — high-gain read amplification (default 4.0)
- `r.ALEV4DGA.LowGain <float>` — low-gain read amplification (default 1.0)
- `r.ALEV4DGA.ShadowThreshold <float>` — luminance below which the merge is
  fully high-gain (default 0.18 — 18% middle grey)
- `r.ALEV4DGA.HighlightThreshold <float>` — luminance above which the merge is
  fully low-gain (default 0.5)
- `r.ALEV4DGA.FullWell <float>` — simulated ADC full-scale; each path clips at
  FullWell / gain (default 460.0)

**Sensor noise**
- `r.ALEV4DGA.NoiseScale <float>` — master noise scale; 0 disables,
  1 = measured ALEXA 35 level (default 1.0)
- `r.ALEV4DGA.NoiseReadSigma <float>` — read-noise floor (default 0.0026)
- `r.ALEV4DGA.NoiseShotK <float>` — shot-noise coefficient k in
  σ = √(read² + k·signal) (default 0.00103)

**Diagnostics**
- `r.ALEV4DGA.DiagLuminance 0|1` — ARRI-convention false-colour exposure view
- `r.ALEV4DGA.SpotMeter 0|1` — centre spot meter; green = 18% grey =
  correct exposure; treat it as a guide rather than a calibrated meter." That's honest and still gives users something.

## Notes on use

**Camera and exposure setup.** In your Post Process Volume, under **Exposure**:

- Set **Metering Mode** to **Manual** for predictable results. Auto-exposure
  modes are handled correctly by the plugin (pre-exposure is divided out and
  re-applied), but they change exposure frame to frame, which makes controlled
  comparisons and repeatable captures difficult.
- Enable **Apply Physical Camera Exposure** so the Cine Camera's aperture,
  shutter and ISO drive exposure the way a physical camera does.

**Other post-processing.** Effects such as bloom, vignette, lens flare and
chromatic aberration are applied by Unreal and may need to be adjusted or
disabled depending on the look you want, as they modify the image around the
simulation. Pre-built sample worlds in particular often ship with aggressive
auto-exposure volumes and heavy volumetric atmospherics that alter the light
before it reaches the simulation.

**Grading.** Output is **LogC4 in AWG4** and will look flat and desaturated
before grading — that is expected. Apply an ARRI LogC4 → Rec.709 transform in
your grading application. Testing used the official ARRI LUT:

- `ARRI_LogC4-to-Gamma24_Rec709-D65_v1_65.cube`
- Available from ARRI's LogC resources:
  https://www.arri.com/en/learn-help/learn-help-camera-system/image-science/log-c

Note this must be a **LogC4** LUT — legacy LogC3 LUTs (including some that ship
with grading software by default) will not decode this footage correctly.

For comparisons and technical detail: https://sciausu.github.io/ALEV4SIM/


## Attribution

Designed and directed by Stefan Ciausu. Sensor architecture, colour-science
research, debugging, and validation by me; shader and plugin implementation
developed with AI assistance (Claude).

Sensor noise model characterised from ARRI ALEXA 35 reference footage
provided by Trevor Calham.

## Disclaimer

This is an independent educational project and is not affiliated with,
endorsed by, or supported by ARRI. ARRI, ALEXA, ALEV, LogC4 and AWG4 are
trademarks of Arnold & Richter Cine Technik GmbH & Co. Betriebs KG. The
LogC4 transfer function and AWG4 primaries are implemented from ARRI's
publicly published specifications.
