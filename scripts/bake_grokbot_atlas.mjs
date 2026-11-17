#!/usr/bin/env node
/**
 * Bake a 4x supersampled anti-aliased 360x360 GrokBot atlas from the vector engine.
 *
 * Usage:
 *   node scripts/bake_grokbot_atlas.mjs [options]
 *
 * Options:
 *   --output <path>        Output atlas path (default: main/boards/m5stack/stopwatch-grokbot/assets/grokbot.atlas)
 *   --frames-dir <path>    Intermediate baked frames directory (default: .tmp/grokbot-frames-1440)
 *   --grokbot-root <path>  Upstream Stopwatch-GrokBot checkout (default: research/Stopwatch-GrokBot)
 *   --chrome <path>        Chrome binary (default: $CHROME_BIN or /opt/google/chrome/google-chrome)
 *   --size <px>            Target square atlas resolution (default: 360)
 *   --supersample <n>      Supersampling factor (default: 4)
 *   --concurrency <n>      Parallel browser workers (default: 4)
 *   --max-size <bytes>     Maximum output atlas byte size (default: 0x700000)
 */

import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, '..');

const XIAOZHI_STATES = [
  'idle',
  'listening',
  'thinking',
  'streaming',
  'searching',
  'radar',
  'waiting',
  'loading',
  'uploading',
  'notifying',
  'alerting',
  'approving',
  'failed',
  'sleeping',
  'waking',
  'happy',
  'excited',
  'laughing',
  'proud',
  'curious',
  'playful',
  'shy',
  'surprised',
  'angry',
  'confused',
  'sad',
  'drowsy',
  'scared',
  'humming',
  'suspicious',
  'finished',
  'celebrate',
  'working',
  'turntable',
  'logo',
  'spawning',
  'writing',
  'receiving',
  'powering-down',
];

function parseArgs(argv) {
  const opts = {
    output: path.join(repoRoot, 'main/boards/m5stack/stopwatch-grokbot/assets/grokbot.atlas'),
    framesDir: path.join(repoRoot, '.tmp/grokbot-frames-1440'),
    grokbotRoot: path.join(repoRoot, 'research/Stopwatch-GrokBot'),
    chrome:
      process.env.CHROME_BIN ||
      (fs.existsSync('/opt/google/chrome/google-chrome')
        ? '/opt/google/chrome/google-chrome'
        : '/usr/bin/google-chrome'),
    size: 360,
    supersample: 4,
    concurrency: 4,
    maxSize: '0x700000',
    checkOnly: false,
  };
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--output') opts.output = path.resolve(argv[++i]);
    else if (arg === '--frames-dir') opts.framesDir = path.resolve(argv[++i]);
    else if (arg === '--grokbot-root') opts.grokbotRoot = path.resolve(argv[++i]);
    else if (arg === '--chrome') opts.chrome = argv[++i];
    else if (arg === '--size') opts.size = Number(argv[++i]);
    else if (arg === '--supersample') opts.supersample = Number(argv[++i]);
    else if (arg === '--concurrency') opts.concurrency = Number(argv[++i]);
    else if (arg === '--max-size') opts.maxSize = argv[++i];
    else if (arg === '--check-upstream') opts.checkOnly = true;
    else throw new Error(`Unknown option: ${arg}`);
  }
  return opts;
}

function loadPuppeteer(grokbotRoot) {
  const candidates = [
    path.join(repoRoot, '.tmp/package.json'),
    path.join(grokbotRoot, 'tools/grokbot/package.json'),
    path.join(repoRoot, 'package.json'),
  ];
  for (const candidate of candidates) {
    if (!fs.existsSync(candidate)) continue;
    try {
      const req = createRequire(candidate);
      return req('puppeteer-core');
    } catch {
      // try next location
    }
  }
  throw new Error(
    'puppeteer-core not found. Install it with: (cd .tmp && pnpm add puppeteer-core)'
  );
}

export function upgradeVectorPrecision(code) {
  const rules = [
    {
      name: 'eye SDF contour tolerances (JM/eT/tT/nT)',
      pattern: /const JM = \.02, eT = 16, tT = \.015, nT = 400\b/g,
      replacement: 'const JM = .0005, eT = 20, tT = .003, nT = 1200',
    },
    {
      name: 'sphere polygon/arc subdivision (yT/kT)',
      pattern: /const yT = 192, kT = Math\.PI \/ 48;/g,
      replacement: 'const yT = 768, kT = Math.PI / 192;',
    },
    {
      name: 'morph angle sampling table (kE)',
      pattern: /const kE = hM\(192\);/g,
      replacement: 'const kE = hM(768);',
    },
    {
      name: 'morph ring polygon ($h 96 -> 384)',
      pattern:
        /(\$h = Array\.from\(\{ length: )96( \}, \(e, t\) => \{[^\n]*\n\s*const n = t \/ )96( \* TAU;)/g,
      replacement: '$1384$2384$3',
    },
  ];

  let updated = code;
  for (const rule of rules) {
    const matches = [...updated.matchAll(rule.pattern)];
    if (matches.length !== 1) {
      throw new Error(
        `grok-player.js patch "${rule.name}" expected exactly 1 match, found ${matches.length}`
      );
    }
    updated = updated.replace(rule.pattern, rule.replacement);
  }
  return updated;
}

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  const playerPath = path.join(opts.grokbotRoot, 'third_party/grokbot/grok-player.js');
  const upgradedPlayerCode = upgradeVectorPrecision(fs.readFileSync(playerPath, 'utf8'));
  if (opts.checkOnly) {
    console.log(`Verified grok-player.js vector precision patches against ${playerPath}`);
    return;
  }

  const puppeteer = loadPuppeteer(opts.grokbotRoot);
  const renderSize = opts.size * opts.supersample;
  const fps = 30;

  fs.mkdirSync(opts.framesDir, { recursive: true });

  const mime = {
    '.html': 'text/html; charset=utf-8',
    '.js': 'text/javascript; charset=utf-8',
    '.json': 'application/json; charset=utf-8',
  };

  const server = http.createServer((req, res) => {
    const url = new URL(req.url, 'http://localhost');
    if (req.method === 'POST' && url.pathname === '/upload') {
      const state = url.searchParams.get('state');
      const chunks = [];
      req.on('data', chunk => chunks.push(chunk));
      req.on('end', () => {
        fs.writeFileSync(path.join(opts.framesDir, `${state}.raw`), Buffer.concat(chunks));
        res.writeHead(200).end('ok');
      });
      return;
    }
    const pathname = decodeURIComponent(url.pathname);
    const file = path.resolve(
      opts.grokbotRoot,
      `.${pathname === '/' ? '/tools/grokbot/render.html' : pathname}`
    );
    if (!file.startsWith(opts.grokbotRoot)) {
      res.writeHead(403).end();
      return;
    }
    fs.readFile(file, (error, data) => {
      if (error) {
        res.writeHead(404).end();
        return;
      }
      const body = pathname.endsWith('grok-player.js')
        ? Buffer.from(upgradedPlayerCode, 'utf8')
        : data;
      res.writeHead(200, {
        'Content-Type': mime[path.extname(file)] || 'application/octet-stream',
      });
      res.end(body);
    });
  });

  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const port = server.address().port;
  const summary = new Array(XIAOZHI_STATES.length);
  let nextIndex = 0;

  try {
    const workers = Array.from({ length: opts.concurrency }, async () => {
      const browser = await puppeteer.launch({
        headless: true,
        executablePath: opts.chrome,
        args: ['--no-sandbox', '--disable-gpu'],
      });
      try {
        const page = await browser.newPage();
        await page.setViewport({ width: renderSize, height: renderSize, deviceScaleFactor: 1 });
        while (true) {
          const idx = nextIndex++;
          if (idx >= XIAOZHI_STATES.length) break;
          const state = XIAOZHI_STATES[idx];
          await page.goto(
            `http://127.0.0.1:${port}/tools/grokbot/render.html?state=${state}&size=${renderSize}&seed=stopwatch`,
            { waitUntil: 'networkidle0' }
          );
          await page.waitForFunction('window.bakerReady === true');
          const info = await page.evaluate(
            async ({ st, targetSize, scale, targetFps }) => {
              const snap = window.bot.debugFrame();
              const count = Math.max(1, Math.round(snap.duration * targetFps));
              const svg = document.querySelector('svg');
              svg.setAttribute('xmlns', 'http://www.w3.org/2000/svg');
              const W = targetSize * scale;
              const H = targetSize * scale;
              const TW = targetSize;
              const TH = targetSize;
              const canvas = document.createElement('canvas');
              canvas.width = W;
              canvas.height = H;
              const ctx = canvas.getContext('2d', { willReadFrequently: true });
              const img = new Image();
              const out = new Uint8Array(count * TW * TH);
              const gray = new Uint8Array(W * H);
              const area = scale * scale;
              const halfArea = area >> 1;

              for (let f = 0; f < count; f++) {
                const xml = new XMLSerializer().serializeToString(svg);
                await new Promise((resolve, reject) => {
                  img.onload = resolve;
                  img.onerror = reject;
                  img.src = 'data:image/svg+xml;charset=utf-8,' + encodeURIComponent(xml);
                });
                ctx.clearRect(0, 0, W, H);
                ctx.drawImage(img, 0, 0, W, H);
                const rgba = ctx.getImageData(0, 0, W, H).data;
                for (let i = 0, j = 0; i < gray.length; i++, j += 4) {
                  const a = rgba[j + 3];
                  gray[i] = a === 0 ? 0 : ((rgba[j] * a + 127) / 255) | 0;
                }
                const frameOff = f * TW * TH;
                for (let ty = 0; ty < TH; ty++) {
                  const baseY = ty * scale;
                  const dstRow = frameOff + ty * TW;
                  for (let tx = 0; tx < TW; tx++) {
                    const baseX = tx * scale;
                    let sum = 0;
                    for (let dy = 0; dy < scale; dy++) {
                      const rowOff = (baseY + dy) * W + baseX;
                      for (let dx = 0; dx < scale; dx++) {
                        sum += gray[rowOff + dx];
                      }
                    }
                    out[dstRow + tx] = ((sum + halfArea) / area) | 0;
                  }
                }
                if (f + 1 < count) window.bot.step(2);
              }
              await fetch(`/upload?state=${encodeURIComponent(st)}`, {
                method: 'POST',
                body: out,
              });
              return { state: st, clip: snap.clip, duration: snap.duration, count, fps: targetFps };
            },
            { st: state, targetSize: opts.size, scale: opts.supersample, targetFps: fps }
          );
          summary[idx] = info;
          console.log(`${state.padEnd(16)} ${info.clip.padEnd(14)} ${String(info.count).padStart(3)} frames`);
        }
      } finally {
        await browser.close();
      }
    });
    await Promise.all(workers);
  } finally {
    server.close();
  }

  fs.writeFileSync(
    path.join(opts.framesDir, 'summary.json'),
    JSON.stringify(
      { fps, size: opts.size, supersample: opts.supersample, states: summary },
      null,
      2
    )
  );

  const encodeResult = spawnSync(
    'uv',
    [
      'run',
      '--with',
      'numpy',
      '--with',
      'pillow',
      'python',
      path.join(here, 'build_grokbot_subset.py'),
      opts.framesDir,
      opts.output,
      '--size',
      String(opts.size),
      '--max-size',
      String(opts.maxSize),
    ],
    { stdio: 'inherit' }
  );
  if (encodeResult.status !== 0) {
    process.exit(encodeResult.status ?? 1);
  }
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main().catch(err => {
    console.error(err);
    process.exit(1);
  });
}
