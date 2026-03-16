#!/usr/bin/env python3
"""Generate a Swedish flag with a Volvo 240 in SIXEL format."""
import sys

W, H = 480, 288

# Pixel buffer: list of (r, g, b) tuples
pixels = [(0, 0, 0)] * (W * H)

def put(x, y, r, g, b):
    if 0 <= x < W and 0 <= y < H:
        pixels[y * W + x] = (r, g, b)

def fill_rect(x1, y1, x2, y2, r, g, b):
    for y in range(max(0, y1), min(H, y2)):
        for x in range(max(0, x1), min(W, x2)):
            pixels[y * W + x] = (r, g, b)

def fill_circle(cx, cy, radius, r, g, b):
    for y in range(max(0, cy - radius), min(H, cy + radius + 1)):
        for x in range(max(0, cx - radius), min(W, cx + radius + 1)):
            if (x - cx) ** 2 + (y - cy) ** 2 <= radius ** 2:
                pixels[y * W + x] = (r, g, b)

# --- Swedish flag ---
# Blue: #006AA7 = (0, 106, 167)
# Yellow: #FECC02 = (254, 204, 2)
BLUE = (0, 106, 167)
YELLOW = (254, 204, 2)

# Flag fills the whole image
fill_rect(0, 0, W, H, *BLUE)

# Swedish cross - offset to the left (Scandinavian style)
# Proportions: flag is 10:16. Cross bars are 2 units wide.
# Vertical bar: at 5/16 from left
# Horizontal bar: at 4/10 from top
cross_w = H * 2 // 10  # width of cross bar in pixels
v_center = W * 5 // 16  # vertical bar center-x
h_center = H // 2       # horizontal bar center-y

# Horizontal bar (full width)
fill_rect(0, h_center - cross_w // 2, W, h_center + cross_w // 2, *YELLOW)
# Vertical bar (full height)
fill_rect(v_center - cross_w // 2, 0, v_center + cross_w // 2, H, *YELLOW)

# --- Volvo 240 --- (side profile, driving right)
# Position the car in the lower portion
car_base_y = 220  # bottom of car body
car_top_y = 175    # top of car body (hood/trunk)
roof_top_y = 148   # top of roof
car_left = 80
car_right = 380

# Colors
BODY = (180, 10, 10)       # Classic red Volvo
BODY_DARK = (140, 8, 8)    # Darker red for detail
TRIM = (40, 40, 40)        # Dark trim
CHROME = (200, 200, 210)   # Chrome/silver
WINDOW = (140, 180, 220)   # Window glass
WINDOW_DARK = (100, 140, 180)
BLACK = (20, 20, 20)
TIRE = (30, 30, 30)
HUBCAP = (180, 180, 190)
WHITE = (240, 240, 240)
ORANGE = (255, 160, 0)
RED_LIGHT = (255, 30, 30)
GROUND = (60, 60, 60)

# Ground/road
fill_rect(0, 235, W, 260, *GROUND)
fill_rect(0, 260, W, H, 50, 50, 50)
# Road markings
fill_rect(30, 246, 90, 250, *YELLOW)
fill_rect(130, 246, 190, 250, *YELLOW)
fill_rect(230, 246, 290, 250, *YELLOW)
fill_rect(330, 246, 390, 250, *YELLOW)
fill_rect(430, 246, 470, 250, *YELLOW)

# Car body - the boxy Volvo 240 shape
# Main body rectangle (very boxy!)
fill_rect(car_left, car_top_y, car_right, car_base_y, *BODY)

# Roof - the classic boxy greenhouse
roof_left = car_left + 80
roof_right = car_right - 60
fill_rect(roof_left, roof_top_y, roof_right, car_top_y, *BODY)

# Slight slope on front windshield pillar
for i in range(car_top_y - roof_top_y):
    x = roof_left - i * 15 // (car_top_y - roof_top_y)
    fill_rect(x, roof_top_y + i, roof_left, roof_top_y + i + 1, *BODY)

# Slight slope on rear window
for i in range(car_top_y - roof_top_y):
    x = roof_right + i * 10 // (car_top_y - roof_top_y)
    fill_rect(roof_right, roof_top_y + i, x, roof_top_y + i + 1, *BODY)

# Windows
win_top = roof_top_y + 5
win_bot = car_top_y - 3

# Front windshield (angled)
for i in range(win_bot - win_top):
    y = win_top + i
    x_left = roof_left - 8 + i * 12 // (win_bot - win_top)
    x_right = roof_left + 38
    fill_rect(x_left, y, x_right, y + 1, *WINDOW)

# B-pillar
fill_rect(roof_left + 40, win_top, roof_left + 48, win_bot, *BODY)

# Rear side window
fill_rect(roof_left + 50, win_top, roof_left + 95, win_bot, *WINDOW)

# C-pillar
fill_rect(roof_left + 97, win_top, roof_left + 108, win_bot, *BODY)

# Rear window (slightly angled)
for i in range(win_bot - win_top):
    y = win_top + i
    x_left = roof_left + 110
    x_right = roof_right - 5 + i * 8 // (win_bot - win_top)
    fill_rect(x_left, y, x_right, y + 1, *WINDOW)

# Window reflection highlights
fill_rect(roof_left + 5, win_top + 3, roof_left + 20, win_top + 5, *WINDOW_DARK)
fill_rect(roof_left + 55, win_top + 3, roof_left + 80, win_top + 5, *WINDOW_DARK)

# Bumpers (chrome, boxy)
fill_rect(car_left - 12, car_base_y - 18, car_left + 5, car_base_y - 6, *CHROME)
fill_rect(car_right - 5, car_base_y - 18, car_right + 15, car_base_y - 6, *CHROME)

# Front details
# Headlight
fill_rect(car_left - 8, car_top_y + 8, car_left + 8, car_top_y + 22, *WHITE)
fill_rect(car_left - 6, car_top_y + 10, car_left + 6, car_top_y + 20, 255, 255, 200)

# Turn signal (orange)
fill_rect(car_left - 8, car_top_y + 24, car_left + 5, car_top_y + 30, *ORANGE)

# Grille (the classic Volvo diagonal slash grille)
for gy in range(car_top_y + 6, car_top_y + 34, 3):
    fill_rect(car_left + 10, gy, car_left + 55, gy + 1, *CHROME)
fill_rect(car_left + 10, car_top_y + 6, car_left + 55, car_top_y + 34, *TRIM)
for gy in range(car_top_y + 8, car_top_y + 32, 4):
    fill_rect(car_left + 12, gy, car_left + 53, gy + 2, *CHROME)
# Diagonal slash
for i in range(20):
    x = car_left + 28 + i
    y1 = car_top_y + 8 + i * 22 // 20
    fill_rect(x, y1, x + 3, y1 + 2, *CHROME)

# Rear details
# Tail light (the iconic vertical tail light)
fill_rect(car_right - 8, car_top_y + 5, car_right + 2, car_top_y + 35, *RED_LIGHT)
fill_rect(car_right - 6, car_top_y + 28, car_right, car_top_y + 35, *ORANGE)

# Body line (the classic Volvo 240 character line)
fill_rect(car_left + 5, car_top_y + 20, car_right - 10, car_top_y + 22, *BODY_DARK)

# Lower body trim
fill_rect(car_left, car_base_y - 5, car_right, car_base_y, *TRIM)

# Door handle
fill_rect(roof_left + 55, car_top_y + 10, roof_left + 70, car_top_y + 13, *CHROME)

# Wheel wells (arches)
fill_circle(car_left + 65, car_base_y, 30, *BLACK)
fill_circle(car_right - 70, car_base_y, 30, *BLACK)

# Tires
fill_circle(car_left + 65, car_base_y + 2, 26, *TIRE)
fill_circle(car_right - 70, car_base_y + 2, 26, *TIRE)

# Hubcaps
fill_circle(car_left + 65, car_base_y + 2, 14, *HUBCAP)
fill_circle(car_right - 70, car_base_y + 2, 14, *HUBCAP)
# Hub center
fill_circle(car_left + 65, car_base_y + 2, 5, *CHROME)
fill_circle(car_right - 70, car_base_y + 2, 5, *CHROME)

# Hubcap spokes
for spoke in range(5):
    import math
    angle = spoke * 2 * math.pi / 5
    for r in range(6, 13):
        sx = int(car_left + 65 + r * math.cos(angle))
        sy = int(car_base_y + 2 + r * math.sin(angle))
        put(sx, sy, *CHROME)
        put(sx + 1, sy, *CHROME)
    # Same for rear wheel
    for r in range(6, 13):
        sx = int(car_right - 70 + r * math.cos(angle))
        sy = int(car_base_y + 2 + r * math.sin(angle))
        put(sx, sy, *CHROME)
        put(sx + 1, sy, *CHROME)

# Motion lines (speed!)
for ly in range(car_top_y + 10, car_base_y - 10, 12):
    lx = car_left - 20
    length = 15 + (ly % 3) * 10
    fill_rect(lx - length, ly, lx - 5, ly + 2, 200, 200, 200)

# Small motion lines near ground
fill_rect(car_left - 40, 232, car_left - 15, 233, 150, 150, 150)
fill_rect(car_left - 55, 228, car_left - 25, 229, 150, 150, 150)

# --- Encode as SIXEL ---
# Collect unique colors
color_set = set()
for p in pixels:
    color_set.add(p)
color_list = sorted(color_set)
color_map = {c: i for i, c in enumerate(color_list)}

out = sys.stdout.buffer

# DCS with raster attributes
out.write(f'\033Pq"1;1;{W};{H}'.encode())

# Define colors (HLS type 2 = RGB, values 0-100)
for i, (r, g, b) in enumerate(color_list):
    out.write(f'#{i};2;{r*100//255};{g*100//255};{b*100//255}'.encode())

# Encode sixel rows (6 pixels high each)
for band in range(0, H, 6):
    # Group by color - only emit colors that appear in this band
    band_colors = set()
    for y in range(band, min(band + 6, H)):
        for x in range(W):
            band_colors.add(color_map[pixels[y * W + x]])

    first_color = True
    for ci in sorted(band_colors):
        out.write(f'#{ci}'.encode())
        row_data = []
        for x in range(W):
            sixel = 0
            for bit in range(6):
                y = band + bit
                if y < H and color_map[pixels[y * W + x]] == ci:
                    sixel |= (1 << bit)
            row_data.append(sixel)

        # RLE encode
        encoded = bytearray()
        i = 0
        while i < len(row_data):
            val = row_data[i]
            count = 1
            while i + count < len(row_data) and row_data[i + count] == val:
                count += 1
            ch = val + 0x3f
            if count >= 4:
                encoded.extend(f'!{count}'.encode())
                encoded.append(ch)
            else:
                encoded.extend(bytes([ch]) * count)
            i += count
        out.write(bytes(encoded))
        out.write(b'$')  # CR

    out.write(b'-')  # newline (next sixel row)

# End DCS
out.write(b'\033\\')
out.write(b'\n')
