import { useLayoutEffect, useRef, type ComponentProps } from 'react'
import { Slider as Primitive } from '@/components/ui/slider'

export function Slider(props: ComponentProps<typeof Primitive>) {
  const ref = useRef<HTMLSpanElement>(null)
  const label = props['aria-label']
  const labelledBy = props['aria-labelledby']
  const count = props.value?.length ?? props.defaultValue?.length ?? 2
  useLayoutEffect(() => {
    // 官方组件把无障碍属性放在根节点 实际交互的是内部滑块
    ref.current?.querySelectorAll('[role="slider"]').forEach((thumb, index) => {
      if (label) thumb.setAttribute('aria-label', count > 1 ? `${label} ${index + 1}` : label)
      if (labelledBy) thumb.setAttribute('aria-labelledby', labelledBy)
    })
  }, [label, labelledBy, count])
  return <Primitive {...props} ref={ref} />
}
