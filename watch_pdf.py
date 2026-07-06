#!/usr/bin/env python3
"""Watch main.pdf and auto-convert to main.pptx on every change.
Speaker notes are parsed from speaker_notes.md and injected per slide.
"""

import io
import re
import sys
import time
from pathlib import Path

from pdf2image import convert_from_path
from pptx import Presentation
from watchdog.events import FileSystemEventHandler
from watchdog.observers import Observer

PDF   = Path(__file__).parent / "main.pdf"
NOTES = Path(__file__).parent / "speaker_notes.md"
OUT   = Path(__file__).parent / "main.pptx"

# Matches:  ## SLIDE 7 — Title (~50 sec)
_SLIDE_RE = re.compile(r"^## SLIDE (\d+)\b", re.MULTILINE)


def parse_notes(md: Path) -> dict[int, str]:
    """Return {slide_number: notes_text} from speaker_notes.md."""
    text = md.read_text(encoding="utf-8")
    result: dict[int, str] = {}

    # Split on horizontal rules so each block is one section
    blocks = re.split(r"\n---\n", text)
    for block in blocks:
        m = _SLIDE_RE.search(block)
        if not m:
            continue
        slide_num = int(m.group(1))
        # Everything after the full ## SLIDE N … header line (skip rest of that line)
        rest = block[m.end():]
        after_header = rest[rest.find("\n"):].lstrip("\n")
        # Strip the timing annotation from the header line if it spills into content
        notes = after_header.strip()
        if notes:
            result[slide_num] = notes

    return result


def convert(pdf: Path, notes_md: Path, out: Path) -> None:
    pages = convert_from_path(str(pdf), dpi=200)
    w, h  = pages[0].size

    notes_map = parse_notes(notes_md) if notes_md.exists() else {}
    if notes_map:
        matched = sum(1 for i in range(1, len(pages) + 1) if i in notes_map)
        print(f"  Notes loaded: {len(notes_map)} sections, "
              f"{matched}/{len(pages)} slides matched")
    else:
        print("  No speaker_notes.md found — skipping notes")

    prs = Presentation()
    prs.slide_width  = int(10.0 * 914400)
    prs.slide_height = int(10.0 * 914400 * h / w)

    blank = prs.slide_layouts[6]

    for i, page in enumerate(pages, start=1):
        slide = prs.slides.add_slide(blank)

        # Embed slide image
        buf = io.BytesIO()
        page.save(buf, format="PNG")
        buf.seek(0)
        slide.shapes.add_picture(buf, 0, 0, prs.slide_width, prs.slide_height)

        # Inject speaker notes
        if i in notes_map:
            slide.notes_slide.notes_text_frame.text = notes_map[i]

    prs.save(str(out))
    print(f"[{time.strftime('%H:%M:%S')}] {len(pages)} slides → {out.name}")


class ChangeHandler(FileSystemEventHandler):
    """Re-convert whenever main.pdf or speaker_notes.md changes."""

    def __init__(self, pdf: Path, notes: Path, out: Path) -> None:
        self.pdf   = pdf
        self.notes = notes
        self.out   = out
        self._last = 0.0  # debounce timestamp

    def on_modified(self, event):
        changed = Path(event.src_path).resolve()
        if changed not in (self.pdf.resolve(), self.notes.resolve()):
            return
        now = time.time()
        if now - self._last < 2.0:
            return
        self._last = now
        trigger = "PDF" if changed == self.pdf.resolve() else "notes"
        print(f"[{time.strftime('%H:%M:%S')}] {trigger} changed, converting...")
        try:
            convert(self.pdf, self.notes, self.out)
        except Exception as exc:
            print(f"  Error: {exc}", file=sys.stderr)


if __name__ == "__main__":
    if not PDF.exists():
        sys.exit(f"Not found: {PDF}")

    print(f"Watching {PDF.name} + {NOTES.name}  (Ctrl-C to stop)")
    print("Running initial conversion...")
    convert(PDF, NOTES, OUT)

    handler  = ChangeHandler(PDF, NOTES, OUT)
    observer = Observer()
    observer.schedule(handler, str(PDF.parent), recursive=False)
    observer.start()
    try:
        while observer.is_alive():
            observer.join(timeout=1)
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        observer.stop()
        observer.join()
