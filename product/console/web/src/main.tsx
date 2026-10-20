import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import App from './App'
import './styles.css'

declare const __AULA_EMULATOR__: boolean

createRoot(document.getElementById('root')!).render(<StrictMode>
  {__AULA_EMULATOR__ ? <div className="emulator-session">
    <aside className="emulator-notice" aria-label="Emulator environment">
      <strong>Local emulator</strong>
      <span>Product console with synthetic calls and activity. Camera and hardware media are unavailable.</span>
    </aside>
    <App />
  </div> : <App />}
</StrictMode>)
