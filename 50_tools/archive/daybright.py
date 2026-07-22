import re
for ln in open("hg_2026-06-21.log", encoding="utf-8", errors="replace"):
    f=[x.strip() for x in ln.rstrip("\n").split("|")]
    if len(f)<3 or f[2]!="SHOT": continue
    try: frame=int(f[3]); lum=float(f[7]); detail=f[8] if len(f)>8 else ""
    except: continue
    if detail.split()[0]!="day": continue
    m=re.search(r"Y=([0-9.]+)\s+ev([+-]?[0-9.]+)", detail)
    if not m: continue
    Y=float(m.group(1)); ev=float(m.group(2))
    # 明るい時間帯(露出が絞られている=lumが低い)を数枚
    if lum <= -7.0:
        # sRGB符号化に戻した中央値(表示上の明るさ)も併記
        # x: srgbToLinear(x)=Y を逆算
        import math
        x = (Y*12.92) if Y<=0.0031308 else (1.055*Y**(1/2.4)-0.055)
        print(f"{f[0][11:]} f{frame} lum{lum:+6.2f} Y={Y:.4f}(線形) 中央値x={x:.3f}(表示) ev{ev:+.2f} iso{f[4]} ss{f[5]} f{f[6]}")
