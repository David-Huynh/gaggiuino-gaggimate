#!/usr/bin/env bash

# Clean data
rm -rf data/w
mkdir -p data/w
mkdir -p data/p

# Build web application
cd web || exit
npm ci
VITE_DISABLE_HARDWARE_SCALE="${GAGGIMATE_DISABLE_HARDWARE_SCALE:-0}" npm run build

cp -R dist/* ../data/w/
gzip ../data/w/assets/*.js
gzip ../data/w/assets/*.css
gzip ../data/w/*.html
