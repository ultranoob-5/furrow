// Furrow High-Performance Web Bundler
// Uses esbuild to bundle, minify, and tree-shake modular Firebase v11
// Outputting self-contained production bundles into web/ for instant offline loading

import * as esbuild from 'esbuild';
import fs from 'fs';
import path from 'path';
import zlib from 'zlib';

console.log('🚀 Building Furrow modular web bundles with esbuild...');
const start = performance.now();

try {
  // 1. Build main dashboard Firebase bundle
  const mainResult = await esbuild.build({
    entryPoints: ['web/src/firebase-entry.js'],
    outfile: 'web/firebase-bundle.js',
    bundle: true,
    minify: true,
    sourcemap: false,
    format: 'iife',
    globalName: 'FurrowFirebaseBundle',
    platform: 'browser',
    target: ['es2020', 'chrome80', 'safari14', 'firefox78', 'edge88'],
    metafile: true,
    legalComments: 'none'
  });

  // 2. Build Service Worker background push Firebase bundle
  const swResult = await esbuild.build({
    entryPoints: ['web/src/sw-firebase-entry.js'],
    outfile: 'web/sw-firebase.js',
    bundle: true,
    minify: true,
    sourcemap: false,
    format: 'iife',
    platform: 'browser',
    target: ['es2020', 'chrome80', 'safari14', 'firefox78', 'edge88'],
    metafile: true,
    legalComments: 'none'
  });

  const duration = (performance.now() - start).toFixed(1);

  // Measure bundle file sizes
  const mainStats = fs.statSync('web/firebase-bundle.js');
  const mainGzip = zlib.gzipSync(fs.readFileSync('web/firebase-bundle.js')).length;

  const swStats = fs.statSync('web/sw-firebase.js');
  const swGzip = zlib.gzipSync(fs.readFileSync('web/sw-firebase.js')).length;

  console.log(`✅ Build completed in ${duration}ms!`);
  console.log(`📦 web/firebase-bundle.js: ${(mainStats.size / 1024).toFixed(1)} KB minified (${(mainGzip / 1024).toFixed(1)} KB gzipped)`);
  console.log(`📦 web/sw-firebase.js:     ${(swStats.size / 1024).toFixed(1)} KB minified (${(swGzip / 1024).toFixed(1)} KB gzipped)`);
  console.log(`🎉 Payload reduction vs legacy 5 CDN compat libraries: ~${(((350 - (mainGzip / 1024)) / 350) * 100).toFixed(0)}% smaller!`);

} catch (err) {
  console.error('❌ Build failed:', err);
  process.exit(1);
}
