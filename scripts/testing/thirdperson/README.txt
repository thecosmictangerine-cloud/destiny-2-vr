Captures are read from %SVR_CAPTURES% (default ../r1). The 2026-10-18 ones: cap_1p (hand cannon), cap_sw (sword), cap_sp (sparrow).
Offline analysis of r1\cap_1p (hand cannon, 1P) and r1\cap_sw (sword, 3P chase camera).
Run from this directory with plain `python <script> [cap_1p|cap_sw]`; captures are read from ..\r1.

cap.py             loader (text+bin), structured per-slot parse of cb/pcb/vb/srv, view and palette helpers
survey.py          Q1: cb11 sizes, distinct cb12 views (camera, projection), VS SRV buffers   -> survey_*.txt
passes.py          pass timeline: runs of draws by render targets + cb12 view, eye from the VP -> passes_*.txt
palettes.py        every rigid-matrix run in any VS cb (bone palettes)                        -> pal_*.txt
body.py            Q2/Q4: palettes rooted within 4 m of the main camera, grouped by (VB, palette), passes -> body_*.txt
cbptr.py           Q6: all palettes keyed by their per-object cb11 buffer pointer, distance to camera -> cbptr_*.txt
skel.py            Q5: prefix test, root frame/yaw, camera offset, joint solve across poses (vs docs/ARMS.md) -> skel_out.txt
keypoints.py       world positions of hips/spine/neck/head/shoulders/wrists/ankles in both captures -> keypoints_out.txt
head.py            head-bone candidates (13/16/18), helmet centre and eye point per bone        -> head_out.txt
sword.py           Q3: weapon palettes (sword, stowed rifle, 3P hand cannon), hand-relative transforms,
                   cb0 dump, scan of all VS/PS constants for points near the sword grip         -> sword_out.txt
shaders_by_pass.py vertex shaders of the player's draws per pass (VM, G-buffer, CSM, forward)
sparrow.py         cap_sp: rider (72-bone family on the sparrow) + sparrow palettes, frames, offsets -> sparrow_out.txt
dynvb.py           every draw reading a DYNAMIC vertex buffer (CPU-written geometry), layout, offsets, pass
nearplayer.py      every draw whose cb11 (palette, 128-B world matrix, or collapsed palette) sits at the player
cloth.py           the class item's CPU-simulated cloth (float4 world-space dynamic VB, 128-B cb11 with the cloak's header) in each capture
project.py         joints, hands, sword and their sun shadows projected into the composed eye image (SVR_seen_*_eye0.bmp)
hammer.py          cap_hammer (Titan, Hammer of Sol): family palettes, cloth, small objects and effects near the player
mark.py            the Titan mark (6-bone family palette): bones in the root frame, projected into the down eye image
