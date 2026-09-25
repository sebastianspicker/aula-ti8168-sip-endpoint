import type { ApiError, ApiResult, CallRequest, DeviceStatus, DirectoryRequest, JsonRecord, Role, Session, SettingsRequest } from './types'
import { isDeviceActivityState, isRecord, roleFromApi } from './types'
import { requestJson } from './request'

const API_BASE = '/zoom/api/v1'
const MUTATING_METHODS = new Set(['POST', 'PATCH', 'PUT', 'DELETE'])
export type OemCredentialsStatus = { revision: number; credentials_present: boolean; connection: string }

export class ConsoleApi {
  private csrfToken: string | null = null

  async session(signal?: AbortSignal): Promise<ApiResult<Session>> {
    const result = await this.request<unknown>('/auth/session', { signal })
    if (!isRecord(result.data)) throw this.invalidResponse('Session role was not returned.')
    const role = roleFromApi(result.data.role)
    const csrf = typeof result.data.csrf_token === 'string' ? result.data.csrf_token : null
    if (!role || !csrf) throw this.invalidResponse('Session state was not recognized.')
    this.csrfToken = csrf
    return { ...result, data: { role } }
  }

  async login(username: string, password: string, signal?: AbortSignal): Promise<ApiResult<Session>> {
    const result = await this.request<unknown>('/auth/login', { method: 'POST', body: { username, password }, auth: false, signal })
    if (!isRecord(result.data)) throw this.invalidResponse('Login response was invalid.')
    const role = roleFromApi(result.data.role)
    const csrf = typeof result.data.csrf_token === 'string' ? result.data.csrf_token : null
    if (!role || !csrf) throw this.invalidResponse('Login response did not establish a session.')
    this.csrfToken = csrf
    return { ...result, data: { role } }
  }

  async bootstrap(username: string, password: string, bootstrap_code: string, signal?: AbortSignal): Promise<ApiResult<JsonRecord>> {
    return this.request<JsonRecord>('/auth/bootstrap', { method: 'POST', body: { username, password, bootstrap_code }, auth: false, signal })
  }

  async logout(signal?: AbortSignal): Promise<void> {
    await this.request('/auth/logout', { signal, method: 'POST', body: {} })
    this.csrfToken = null
  }

  async deviceStatus(signal?: AbortSignal): Promise<ApiResult<DeviceStatus>> {
    const result = await this.request<unknown>('/device/status', { signal })
    const data = result.data
    if (!isRecord(data) || Object.keys(data).length !== 3 || data.revision !== 1 ||
        !isDeviceActivityState(data.recording) || !isDeviceActivityState(data.streaming)) {
      throw this.invalidResponse('Device activity state was not recognized.')
    }
    return { ...result, data: { revision: 1, recording: data.recording, streaming: data.streaming } }
  }
  library = (signal?: AbortSignal) => this.request<JsonRecord>('/library', { signal })
  async oemCredentials(signal?: AbortSignal): Promise<ApiResult<OemCredentialsStatus>> {
    const result = await this.request<unknown>('/device/credentials', { signal })
    const data = result.data
    if (!isRecord(data) || Object.keys(data).length !== 3 || !Number.isInteger(data.revision) ||
        typeof data.revision !== 'number' || data.revision < 1 || data.revision > 33 ||
        typeof data.credentials_present !== 'boolean' || typeof data.connection !== 'string' ||
        !['unknown', 'connected', 'authentication_failed', 'backoff', 'unconfigured'].includes(data.connection)) {
      throw this.invalidResponse('Device connection status was not recognized.')
    }
    return { ...result, data: data as OemCredentialsStatus }
  }
  saveOemCredentials = (username: string, password: string, revision: number) =>
    this.request<OemCredentialsStatus>('/device/credentials', { method: 'PUT', body: { username, password, revision } })
  schedule = (signal?: AbortSignal) => this.request<JsonRecord>('/schedule', { signal })
  deviceSettings = (signal?: AbortSignal) => this.request<JsonRecord>('/device/settings', { signal })

  status = (signal?: AbortSignal) => this.request<JsonRecord>('/status', { signal })
  events = (signal?: AbortSignal) => this.request<JsonRecord>('/events', { signal })
  calls = (signal?: AbortSignal) => this.request<JsonRecord>('/calls', { signal })
  activeCall = (signal?: AbortSignal) => this.request<JsonRecord>('/calls/active', { signal })
  directory = (signal?: AbortSignal) => this.request<JsonRecord>('/directory', { signal })
  saveDirectory = (entry: DirectoryRequest, revision: number, signal?: AbortSignal) =>
    this.request<JsonRecord>('/directory', { signal, method: 'POST', body: { ...entry, revision } })
  deleteDirectory = (id: string, revision: number, signal?: AbortSignal) =>
    this.request<JsonRecord>('/directory', { signal, method: 'DELETE', body: { id, revision } })
  media = (signal?: AbortSignal) => this.request<JsonRecord>('/media', { signal })
  preview = (signal?: AbortSignal) => this.request<JsonRecord>('/media/preview', { signal })
  previewStreamHeaders(): Readonly<Record<string, string>> | null {
    return this.csrfToken === null ? null : { 'X-CSRF-Token': this.csrfToken }
  }
  settings = (signal?: AbortSignal) => this.request<JsonRecord>('/settings', { signal })
  users = (signal?: AbortSignal) => this.request<JsonRecord>('/users', { signal })
  saveUser = (username: string, password: string, role: Role, revision: number, signal?: AbortSignal) =>
    this.request<JsonRecord>('/users', { signal, method: 'POST', body: { username, password, role, revision } })
  deleteUser = (username: string, revision: number, signal?: AbortSignal) =>
    this.request<JsonRecord>('/users', { signal, method: 'DELETE', body: { username, revision } })
  diagnostics = (signal?: AbortSignal) => this.request<JsonRecord>('/diagnostics', { signal })
  metrics = (signal?: AbortSignal) => this.request<JsonRecord>('/diagnostics/metrics', { signal })
  runDiagnostics = (signal?: AbortSignal) => this.request<JsonRecord>('/diagnostics/tests', { signal, method: 'POST', body: {} })
  exportDiagnostics = (id: string, signal?: AbortSignal) => this.request<JsonRecord>(`/diagnostics/export/${id}`, { signal })
  joinCall = (body: CallRequest, signal?: AbortSignal) => this.request<JsonRecord>('/calls', { signal, method: 'POST', body })
  hangup = (signal?: AbortSignal) => this.request<JsonRecord>('/calls/active', { signal, method: 'DELETE', body: {} })
  sendDtmf = (tone: string, signal?: AbortSignal) => this.request<JsonRecord>('/calls/active/dtmf', { signal, method: 'POST', body: { tone } })
  setVideo = (mode: 'enabled' | 'disabled', signal?: AbortSignal) => this.request<JsonRecord>('/calls/active/media', { signal, method: 'PATCH', body: { mode } })
  setAudioMute = (muted: boolean, signal?: AbortSignal) => this.request<JsonRecord>('/calls/active/media', { signal,
    method: 'PATCH', body: { action: 'audio_mute', value: muted ? 'enabled' : 'disabled' }
  })
  requestKeyframe = (signal?: AbortSignal) => this.request<JsonRecord>('/calls/active/media', { signal,
    method: 'PATCH', body: { action: 'keyframe', value: 'request' }
  })
  cycleLayout = (signal?: AbortSignal) => this.request<JsonRecord>('/calls/active/media', { signal,
    method: 'PATCH', body: { action: 'layout_next', value: 'request' }
  })
  saveSettings = (body: SettingsRequest, signal?: AbortSignal) => this.request<JsonRecord>('/settings', { signal, method: 'PATCH', body })
  replaceCredentials = (username: string, password: string, signal?: AbortSignal) => this.request<JsonRecord>('/settings/credentials', { signal, method: 'POST', body: { username, password } })

  private async request<T>(path: string, init: { method?: string; body?: unknown; auth?: boolean; signal?: AbortSignal } = {}): Promise<ApiResult<T>> {
    const method = init.method ?? 'GET'
    const headers: HeadersInit = { Accept: 'application/json' }
    const operationId = MUTATING_METHODS.has(method) ? crypto.randomUUID() : null
    if (init.body !== undefined) headers['Content-Type'] = 'application/json'
    if (MUTATING_METHODS.has(method)) {
      if (this.csrfToken) headers['X-CSRF-Token'] = this.csrfToken
      headers['Idempotency-Key'] = operationId!
    }
    return requestJson<T>(`${API_BASE}${path}`, {
      method, headers,
      body: init.body === undefined ? undefined : JSON.stringify(init.body),
      credentials: 'same-origin'
    }, operationId !== null && init.auth !== false && this.csrfToken !== null, init.signal)
  }

  private invalidResponse(message: string): ApiError {
    return { code: 'BACKEND_PROTOCOL', message, status: 502 }
  }
}

export const api = new ConsoleApi()
