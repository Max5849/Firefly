/// <reference types="vite/client" />

interface ImportMetaEnv {
  /** Absolute API base. Unset in dev, where Vite proxies /api to 127.0.0.1:8080. */
  readonly VITE_API_URL?: string;
  /** Bearer token for the nginx-fronted deployment. Unset in dev. */
  readonly VITE_API_TOKEN?: string;
}

interface ImportMeta {
  readonly env: ImportMetaEnv;
}
