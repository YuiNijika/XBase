import { toast } from 'sonner'
import { Button } from '@/components/ui/button'
import { toast as baseToast } from '@/components/ui/toast'

interface Props {
  title: string
  description?: string
  variant?: 'success' | 'error' | 'warning' | 'info'
  children?: React.ReactNode
  disabled?: boolean
  engine?: 'sonner' | 'base'
}

export function ToastButton({ title, description, variant = 'info', children, disabled, engine = 'sonner' }: Props) {
  return <Button disabled={disabled} onClick={() => {
    if (engine === 'base') baseToast.add({ title, description, type: variant })
    else toast[variant](title, { description })
  }}>{children ?? title}</Button>
}
