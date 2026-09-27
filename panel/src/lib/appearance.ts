import { useCallback, useEffect, useState } from 'react'

export type ThemeMode = 'system' | 'light' | 'dark'

const ThemeKey = 'xbase.panel.theme'
const HueKey = 'xbase.panel.hue'

const DefaultHue = 260

// 面板资源走虚拟主机加载，localStorage 的域就是那个虚拟域，够用且不依赖原生配置
function readStored(key: string): string {
  try {
    return window.localStorage.getItem(key) ?? ''
  } catch {
    return ''
  }
}

function writeStored(key: string, value: string): void {
  try {
    window.localStorage.setItem(key, value)
  } catch {
    /* 隐私模式下写不进去，界面照常工作 */
  }
}

function systemPrefersDark(): boolean {
  return window.matchMedia?.('(prefers-color-scheme: dark)').matches ?? true
}

export function resolveDark(mode: ThemeMode): boolean {
  return mode === 'dark' || (mode === 'system' && systemPrefersDark())
}

export function useAppearance() {
  const storedTheme = readStored(ThemeKey)
  const [theme, setThemeState] = useState<ThemeMode>(
    storedTheme === 'light' || storedTheme === 'dark' || storedTheme === 'system' ? storedTheme : 'system',
  )
  const [hue, setHueState] = useState<number>(() => {
    const parsed = Number(readStored(HueKey))
    return Number.isFinite(parsed) && parsed >= 0 && parsed <= 359 ? parsed : DefaultHue
  })

  // 明暗挂在根节点上，所有面板内的 token 都从 .dark 派生
  useEffect(() => {
    document.documentElement.classList.toggle('dark', resolveDark(theme))
    if (theme !== 'system') return

    const query = window.matchMedia('(prefers-color-scheme: dark)')
    const sync = () => document.documentElement.classList.toggle('dark', query.matches)
    query.addEventListener('change', sync)
    return () => query.removeEventListener('change', sync)
  }, [theme])

  useEffect(() => {
    document.documentElement.style.setProperty('--accent-hue', String(hue))
  }, [hue])

  const setTheme = useCallback((next: ThemeMode) => {
    setThemeState(next)
    writeStored(ThemeKey, next)
  }, [])

  const setHue = useCallback((next: number) => {
    setHueState(next)
    writeStored(HueKey, String(next))
  }, [])

  return { theme, hue, setTheme, setHue }
}
