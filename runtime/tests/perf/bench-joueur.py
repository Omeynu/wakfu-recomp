# bench-joueur.py <étiquette> [VAR=val...] : sauvegarde du joueur, Continue Game, img/s sur 600 images après g0
# (première ligne [natif N] à 1000 draws ou plus ; lignes toutes les 60 images)
import os, re, subprocess, sys, time, shutil
W = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../work'); S = os.environ.get('BENCH_DIR', '/tmp/wakfu-bench'); os.makedirs(S, exist_ok=True)
tag = sys.argv[1]; data = f'{S}/b2-{tag}'; shutil.rmtree(data, ignore_errors=True); shutil.copytree(f'{W}/joueur-sauvegarde', data)
env = dict(os.environ, VK_ICD_FILENAMES='/usr/share/vulkan/icd.d/radeon_icd.json', WAKFU_RENDER='1', WAKFU_SKIP_VIDEOS='1',
           WAKFU_STOP_AT_SWAP='7000', WAKFU_INPUT='15:START,25:A,32:A,39:A,46:A' + ''.join(f',{t}:A' for t in range(60, 200, 5)))
for kv in sys.argv[2:]: k, v = kv.split('=', 1); env[k] = v
p = subprocess.Popen([os.environ.get('BIN', 'build-runtime/wakfu'), 'game/patched/image.bin', 'game/patched/default.xex', 'game/base', 'game/tu2', data],
                     cwd=W, env=env, stderr=subprocess.PIPE, stdout=subprocess.DEVNULL, text=True, errors='replace')
import signal, threading
# Délai (BENCH_TIMEOUT s, défaut 900) : le jeu ignore SIGTERM, on le tue (SIGKILL) pour libérer le verrou.
killer = threading.Timer(int(os.environ.get('BENCH_TIMEOUT', '900')), lambda: (print(f'{tag} DÉLAI DÉPASSÉ', flush=True), p.kill()))
killer.daemon = True; killer.start()
times, g0, last = {}, None, ''
log = open(f'{data}.log', 'w')
for line in p.stderr:
    log.write(line)
    if line.startswith('[diag]'): print(line, end='', flush=True)
    m = re.match(r'\[natif (\d+)\] draws (\d+)', line)
    if m:
        n, d = int(m[1]), int(m[2]); times[n] = time.monotonic()
        if g0 is None and d >= 1000: g0 = n
    if 'par image (60 dernières)' in line: last = line.split(' : ', 1)[1].strip()
p.wait(); killer.cancel(); log.close()
if g0 is None or g0 + 660 not in times: print(f'{tag} ÉCHEC g0={g0}'); sys.exit(1)
print(f'{tag} g0={g0} img/s={600 / (times[g0 + 660] - times[g0 + 60]):.2f} ; {last}')
