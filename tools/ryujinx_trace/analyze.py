"""Summarises an opencarbon Ryujinx audio trace (testresults/ryujinx_audio/<run>).

usage: analyze.py <run dir> [--session N] [--around-ms T]

Writes WAV files next to the raw dumps (s<N>_guest.wav, s<N>_output.wav) and prints, per session: format, the guest
buffers with their stats, SetVolume/Start calls, and the backend callbacks that played non-silent audio.
"""
import argparse, glob, os, re, struct, wave
from collections import defaultdict

LINE = re.compile(r'^\s*(?P<ms>[\d.]+) t(?P<thread>\d+)\s+(?P<text>.*)$')


def write_wav(pcm_path, wav_path, rate, channels):
    data = open(pcm_path, 'rb').read()
    with wave.open(wav_path, 'wb') as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--session', type=int, action='append')
    ap.add_argument('--limit', type=int, default=40)
    args = ap.parse_args()

    events = []
    for line in open(os.path.join(args.run, 'events.log'), encoding='utf-8-sig'):
        m = LINE.match(line)
        if m:
            events.append((float(m['ms']), int(m['thread']), m['text']))

    sessions = {}
    wraps = {}
    per_session = defaultdict(list)
    for ms, thread, text in events:
        m = re.match(r'session (\d+) created: (\w+) (\d+) Hz, (\d+) ch, (\w+)', text)
        if m:
            sessions[int(m[1])] = (m[2], int(m[3]), int(m[4]), m[5], ms)
        m = re.match(r'session (\d+) \(compat.*wraps session (\d+)', text)
        if m:
            wraps[int(m[1])] = int(m[2])
        m = re.match(r'session (-?\d+) ', text)
        if m:
            per_session[int(m[1])].append((ms, thread, text))
        elif text.startswith('image'):
            print(f'{ms:10.3f} {text}')

    print('\nsessions:')
    for sid, (kind, rate, ch, fmt, ms) in sorted(sessions.items()):
        guest = [e for e in per_session[sid] if 'guest buffer #' in e[2] and 'memory window' not in e[2]]
        vols = [e for e in per_session[sid] if 'SetVolume' in e[2]]
        extra = f' -> real session {wraps[sid]}' if sid in wraps else ''
        print(f'  {sid}: {kind} {rate} Hz {ch} ch {fmt} created at {ms:.1f} ms{extra}; {len(guest)} guest buffers, {len(vols)} SetVolume calls')
        for pcm in glob.glob(os.path.join(args.run, f's{sid}_*.pcm')):
            write_wav(pcm, pcm[:-4] + '.wav', rate, ch)

    for sid in args.session or []:
        print(f'\nsession {sid} (and its real session {wraps.get(sid)}):')
        rows = sorted(per_session[sid] + per_session.get(wraps.get(sid, -99), []))
        shown = 0
        for ms, thread, text in rows:
            quiet = re.search(r'peak 0 nonzero 0/', text) and 'callback' in text
            if quiet and shown > 5:
                continue
            print(f'  {ms:10.3f} t{thread:<3} {text[:220]}')
            shown += 1
            if shown >= args.limit:
                break


if __name__ == '__main__':
    main()
