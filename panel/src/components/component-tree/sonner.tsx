import { useEffect, useState, type ComponentProps } from 'react'
import { Toaster as Primitive } from '@/components/ui/sonner'

export function SonnerToaster(props: ComponentProps<typeof Primitive>) {
  const [theme, setTheme] = useState<'light' | 'dark'>(() => document.documentElement.classList.contains('dark') ? 'dark' : 'light')
  useEffect(() => {
    const observer = new MutationObserver(() => setTheme(document.documentElement.classList.contains('dark') ? 'dark' : 'light'))
    observer.observe(document.documentElement, { attributes: true, attributeFilter: ['class'] })
    return () => observer.disconnect()
  }, [])
  return <Primitive {...props} theme={props.theme ?? theme} />
}
