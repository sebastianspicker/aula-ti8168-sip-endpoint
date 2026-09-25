export type Role = 'viewer' | 'operator' | 'admin'
export type Page = 'room' | 'library' | 'schedule' | 'calls' | 'directory' | 'media' | 'settings' | 'diagnostics'

export type ApiError = {
  code: string
  message: string
  status: number
}

export type ApiResult<T> = { data: T; revision: number }
export type Session = { role: Role }
export type JsonRecord = Record<string, unknown>
export type DeviceActivityState = 'active' | 'inactive' | 'paused' | 'unknown'
export type DeviceStatus = {
  revision: 1
  recording: DeviceActivityState
  streaming: DeviceActivityState
}

export const isDeviceActivityState = (value: unknown): value is DeviceActivityState =>
  value === 'active' || value === 'inactive' || value === 'paused' || value === 'unknown'

export type CallRequest = {
  meeting_id: string
  profile: 'zoom_direct' | 'zoom_proxy' | 'private_lab'
  passcode: string
  layout: 'gallery' | 'full_screen' | 'dual_video'
  host_key: string
  dial_code: string
}

export type DirectoryRequest = {
  id: string
  name: string
  meeting_id: string
  profile: CallRequest['profile']
  default_layout: CallRequest['layout']
}

export type SettingsRequest = {
  revision: number
  profile: CallRequest['profile']
  media: 'managed' | 'disabled'
}

export const PAGE_LABELS: Record<Page, string> = {
  room: 'Room',
  library: 'Library',
  schedule: 'Schedule',
  calls: 'Calls',
  directory: 'Directory',
  media: 'Media',
  settings: 'Settings',
  diagnostics: 'Diagnostics'
}

export const roleFromApi = (value: unknown): Role | null => {
  if (value === 1 || value === 'viewer') return 'viewer'
  if (value === 2 || value === 'operator') return 'operator'
  if (value === 3 || value === 'admin') return 'admin'
  return null
}

export const isRecord = (value: unknown): value is JsonRecord =>
  typeof value === 'object' && value !== null && !Array.isArray(value)

export const readText = (record: JsonRecord | null | undefined, key: string): string | null => {
  const value = record?.[key]
  return typeof value === 'string' && value.trim() ? value : null
}

export const readNumber = (record: JsonRecord | null | undefined, key: string): number | null => {
  const value = record?.[key]
  return typeof value === 'number' && Number.isFinite(value) ? value : null
}
