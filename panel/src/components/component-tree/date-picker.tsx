import { CalendarIcon } from 'lucide-react'
import { Button } from '@/components/ui/button'
import { Calendar } from '@/components/ui/calendar'
import { Popover, PopoverContent, PopoverTrigger } from '@/components/ui/popover'
import { localDate } from './index'

interface Props {
  value?: string
  onValueChange?: (value: string) => void
  disabled?: boolean
  placeholder?: string
}

export function DatePicker({ value, onValueChange, disabled, placeholder = '选择日期' }: Props) {
  const parsed = value ? new Date(`${value}T00:00:00`) : undefined
  const selected = parsed && !Number.isNaN(parsed.getTime()) ? parsed : undefined
  return (
    <Popover>
      <PopoverTrigger asChild>
        <Button variant="outline" disabled={disabled}>
          <CalendarIcon aria-hidden="true" />{selected ? selected.toLocaleDateString() : placeholder}
        </Button>
      </PopoverTrigger>
      <PopoverContent className="w-auto p-0">
        <Calendar mode="single" selected={selected} disabled={disabled} onSelect={(date) => onValueChange?.(date ? localDate(date) : '')} />
      </PopoverContent>
    </Popover>
  )
}
