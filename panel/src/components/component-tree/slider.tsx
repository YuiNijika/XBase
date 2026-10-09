import { useLayoutEffect, useRef, type ComponentProps } from 'react'
import { Slider as Primitive } from '@/components/ui/slider'

export function Slider(props: ComponentProps<typeof Primitive>) {
  const ref = useRef<HTMLSpanElement>(null)
  const label = props['aria-label']
  const labelledBy = props['aria-labelledby']
  const count = props.value?.length ?? props.defaultValue?.length ?? 2
  useLayoutEffect(() => {
    const root = ref.current
    if (!root) return
    const sync = () => root.querySelectorAll('[role="slider"]').forEach((thumb, index) => {
      const name = count > 1 ? `${label} ${index + 1}` : label
      if (label && thumb.getAttribute('aria-label') !== name) thumb.setAttribute('aria-label', name as string)
      if (labelledBy && thumb.getAttribute('aria-labelledby') !== labelledBy) thumb.setAttribute('aria-labelledby', labelledBy)
    })
    // 内部滑块会在重渲染时恢复默认名称 所以需要跟随属性变化同步
    const observer = new MutationObserver(sync)
    observer.observe(root, { subtree: true, childList: true, attributes: true, attributeFilter: ['aria-label', 'aria-labelledby'] })
    sync()
    return () => observer.disconnect()
  }, [label, labelledBy, count])
  return <Primitive {...props} ref={ref} />
}
