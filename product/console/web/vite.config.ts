import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

export default defineConfig(({ mode }) => ({
  base: '/zoom/',
  define: { __AULA_EMULATOR__: JSON.stringify(mode === 'emulator') },
  plugins: [react()]
}))
