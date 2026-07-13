import { defineConfig } from 'vite';
import preact from '@preact/preset-vite';
import tailwindcss from '@tailwindcss/vite';

const disableHardwareScale =
  process.env.GAGGIMATE_DISABLE_HARDWARE_SCALE === '1' ||
  process.env.VITE_DISABLE_HARDWARE_SCALE === '1';

// https://vitejs.dev/config/
export default defineConfig({
  plugins: [preact(), tailwindcss()],
  define: {
    'import.meta.env.VITE_DISABLE_HARDWARE_SCALE': JSON.stringify(
      disableHardwareScale ? '1' : '0',
    ),
  },

  build: {
    // SPIFFS on the firmware caps path length at 32 chars and silently drops
    // files whose path exceeds it. Default Vite chunk names like
    // `assets/chartjs-plugin-annotation.esm-_AL1m4_O.js.gz` blow past that and
    // never make it to the device, leaving the served index.html referencing
    // nonexistent JS files. Emit hash-only filenames so every asset path stays
    // well under the limit.
    rollupOptions: {
      output: {
        entryFileNames: 'assets/[hash].js',
        chunkFileNames: 'assets/[hash].js',
        assetFileNames: 'assets/[hash][extname]',
      },
    },
  },

  server: {
    proxy: {
      '/api': {
        target: 'http://localhost:8080',
        changeOrigin: true,
      },
      '/ws': {
        target: 'ws://localhost:8080',
        ws: true,
      },
    },
    watch: {
      usePolling: true,
    },
  },
});
