import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import tailwindcss from '@tailwindcss/vite';

// The config runs under Node; this project carries no @types/node for one variable.
declare const process: { env: Record<string, string | undefined> };

// In dev the browser talks to Vite, which forwards /api to the strategy server on
// 127.0.0.1:8080 - so the page is same-origin and needs neither CORS nor a token.
// Point it at an SSH tunnel (DEPLOY.md section 9) to drive the Oracle VM instead.
// API_PORT moves it, so a local server and a tunnel on 8080 can run side by side:
//   API_PORT=8081 npm run dev
export default defineConfig({
  plugins: [react(), tailwindcss()],
  server: {
    proxy: {
      '/api': {
        target: `http://127.0.0.1:${process.env.API_PORT ?? '8080'}`,
        changeOrigin: true,
        rewrite: (path) => path.replace(/^\/api/, ''),
      },
    },
  },
});
