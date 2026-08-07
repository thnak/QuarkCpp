import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// The site is served from GitHub Pages out of /docs on the default branch.
// `base: './'` keeps every asset reference relative, so the build works at
// https://thnak.github.io/QuarkCpp/ and from a plain local file server alike.
export default defineConfig({
  plugins: [react()],
  base: './',
  build: {
    outDir: '../docs',
    emptyOutDir: true,
    assetsDir: 'assets',
    chunkSizeWarningLimit: 900,
  },
})
