"""Render documentation graphics from recorded validation results (optional Pillow).

The summary is a generated visualization, not a runtime screenshot. SDL frames
are copied losslessly into the montage and checked against their pixel hashes.
"""
import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

BACKGROUND = '#101923'
PANEL = '#1a2938'
TEXT = '#e8f0f8'
MUTED = '#a9bdcf'
GREEN = '#6de3b2'
BLUE = '#73bafa'


def font(size):
    for name in ('DejaVuSans.ttf', 'C:/Windows/Fonts/segoeui.ttf'):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default(size=size)


def summary(data, output):
    image = Image.new('RGB', (1600, 1050), BACKGROUND)
    draw = ImageDraw.Draw(image)
    draw.text((56, 38), 'Vantage OS | integrated system verification', font=font(40), fill=TEXT)
    draw.text((56, 100), 'Recorded software tests — Windows host + linked AArch64 models', font=font(25), fill=MUTED)
    draw.text((56, 143), 'Tested source: ' + data['tested_source_commit'] + ' | ' + data['date_utc'], font=font(20), fill=MUTED)
    for i, run in enumerate(data['summaries']):
        x, y = 56 + (i % 3) * 504, 212 + (i // 3) * 206
        draw.rounded_rectangle((x, y, x + 476, y + 176), radius=16, fill=PANEL)
        draw.text((x + 23, y + 18), run['label'], font=font(24), fill=BLUE)
        draw.text((x + 23, y + 57), f"{run['passed']}/{run['total']} passed", font=font(37), fill=GREEN)
        draw.text((x + 23, y + 118), run['note'], font=font(19), fill=MUTED)
    fault = data['fault_injection']
    draw.rounded_rectangle((56, 660, 1544, 836), radius=16, fill=PANEL)
    draw.text((80, 680), 'VSTORE3 commit-failure recovery', font=font(27), fill=BLUE)
    draw.text((80, 728), f"{fault['cases']} failure injections", font=font(37), fill=GREEN)
    draw.text((80, 785), f"{fault['cut_points']} I/O cut points x {len(fault['torn_lengths'])} torn lengths x {len(fault['device_modes'])} device modes", font=font(22), fill=MUTED)
    draw.text((56, 870), '256 entries | exact 255-byte paths | 1 MiB files | persistent empty directories', font=font(25), fill=TEXT)
    draw.text((56, 916), 'Generated summary. Raw logs, commands, source identity and hashes accompany this image.', font=font(22), fill=MUTED)
    draw.text((56, 960), 'Physical Pi validation and performance measurements remain pending.', font=font(25), fill='#f2c878')
    image.save(output / 'summary.png')


def montage(data, output):
    image = Image.new('RGB', (1600, 1020), BACKGROUND)
    draw = ImageDraw.Draw(image)
    draw.text((56, 26), 'Actual SDL framebuffer captures | persistent terminal workflow', font=font(33), fill=TEXT)
    draw.text((56, 78), '480 x 800 frames, unchanged pixels. Host simulator captures — not Raspberry Pi photographs.', font=font(22), fill=MUTED)
    captions = [('editor', '1. Editor: nested file saved'), ('folders', '2. Directory listing and file read'),
                ('reboot', '3. Fresh process: persisted data')]
    for i, (name, title) in enumerate(captions):
        record = data['frames'][name]
        frame = Image.open(output / record['file']).convert('RGB')
        if frame.size != (480, 800) or hashlib.sha256(frame.tobytes()).hexdigest() != record['pixel_sha256']:
            raise ValueError('Capture pixels do not match the recorded source: ' + name)
        x = 56 + i * 504
        draw.text((x, 132), title, font=font(23), fill=BLUE)
        image.paste(frame, (x, 180))
    image.save(output / 'simulator-workflow.png')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--results', type=Path, required=True)
    args = parser.parse_args()
    data = json.loads(args.results.read_text(encoding='utf-8'))
    if any(run['passed'] != run['total'] for run in data['summaries']):
        raise ValueError('Cannot render an all-passing summary for incomplete results')
    if any(run['exit_code'] != 0 for run in data['commands']):
        raise ValueError('Recorded validation contains a failed command')
    summary(data, args.results.parent)
    montage(data, args.results.parent)


if __name__ == '__main__':
    main()
