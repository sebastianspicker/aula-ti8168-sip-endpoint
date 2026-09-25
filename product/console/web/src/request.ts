import type { ApiError, ApiResult } from './types'
import { isRecord } from './types'

const ATTEMPT_TIMEOUT_MS = 10_000
const RETRY_CODES = new Set(['NETWORK_UNAVAILABLE', 'REQUEST_TIMEOUT', 'BACKEND_UNAVAILABLE'])

function interruption(code: 'REQUEST_TIMEOUT' | 'REQUEST_ABORTED'): ApiError {
  return { code, message: code === 'REQUEST_TIMEOUT' ? 'The device request timed out.' : 'The device request was cancelled.', status: 0 }
}

function decode<T>(response: Response, payload: unknown): ApiResult<T> {
  if (!isRecord(payload)) throw { code: 'BACKEND_PROTOCOL', message: 'The device response was invalid.', status: 502 } satisfies ApiError
  if (!response.ok || payload.ok === false) {
    const error = isRecord(payload.error) ? payload.error : null
    throw {
      code: typeof error?.code === 'string' ? error.code : `HTTP_${response.status}`,
      message: typeof error?.message === 'string' ? error.message : 'The device rejected this request.',
      status: response.status
    } satisfies ApiError
  }
  if (payload.revision !== 1 || payload.ok !== true || !('data' in payload)) {
    throw { code: 'BACKEND_PROTOCOL', message: 'The device response was invalid.', status: 502 } satisfies ApiError
  }
  return { data: payload.data as T, revision: payload.revision }
}

async function readJson<T>(url: string, init: RequestInit): Promise<ApiResult<T>> {
  let response: Response
  let payload: unknown
  try {
    response = await fetch(url, init)
    payload = await response.json()
  } catch (error) {
    if (error instanceof SyntaxError) throw { code: 'BACKEND_PROTOCOL', message: 'The device response was invalid.', status: 502 } satisfies ApiError
    throw { code: 'NETWORK_UNAVAILABLE', message: 'The console cannot reach the device.', status: 0 } satisfies ApiError
  }
  return decode<T>(response, payload)
}

async function attempt<T>(url: string, init: RequestInit, signal?: AbortSignal): Promise<ApiResult<T>> {
  if (signal?.aborted) throw interruption('REQUEST_ABORTED')
  const controller = new AbortController()
  let cancel = () => {}
  let timer: ReturnType<typeof setTimeout> | undefined
  const interrupted = new Promise<never>((_, reject) => {
    const stop = (code: 'REQUEST_TIMEOUT' | 'REQUEST_ABORTED') => {
      reject(interruption(code))
      controller.abort()
    }
    cancel = () => stop('REQUEST_ABORTED')
    signal?.addEventListener('abort', cancel, { once: true })
    timer = setTimeout(() => stop('REQUEST_TIMEOUT'), ATTEMPT_TIMEOUT_MS)
  })
  try {
    return await Promise.race([readJson<T>(url, { ...init, signal: controller.signal }), interrupted])
  } finally {
    clearTimeout(timer)
    signal?.removeEventListener('abort', cancel)
  }
}

export async function requestJson<T>(url: string, init: RequestInit, retryable: boolean, signal?: AbortSignal): Promise<ApiResult<T>> {
  for (let number = 0; ; number += 1) {
    try {
      return await attempt<T>(url, init, signal)
    } catch (error) {
      if (signal?.aborted) throw interruption('REQUEST_ABORTED')
      const record = isRecord(error) ? error : null
      const code = record?.code
      const transient = typeof code === 'string' && RETRY_CODES.has(code)
      const unavailable = code === 'BACKEND_UNAVAILABLE'
      if (!retryable || number > 0 || !transient || (unavailable && record?.status !== 503)) throw error
    }
  }
}
