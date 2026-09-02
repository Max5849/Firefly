import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import tailwindcss from '@tailwindcss/vite';

// In dev the browser talks to Vite, which forwards /api to the strategy server on
// 127.0.0.1:8080 - so the page is same-origin and needs neither CORS nor a token.
// Point it at an SSH tunnel (DEPLOY.md section 9) to drive the Oracle VM instead.
export default defineConfig({
  plugins: [react(), tailwindcss()],
  server: {
    proxy: {
      '/api': {
        target: 'http://127.0.0.1:8080',
        changeOrigin: true,
        rewrite: (path) => path.replace(/^\/api/, ''),
      },
    },
  },
});
