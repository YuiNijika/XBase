import { Children, cloneElement, isValidElement, type ReactElement, type ReactNode } from 'react'
import { useForm, type ControllerRenderProps, type FieldValues, type RegisterOptions } from 'react-hook-form'
import { Form as FormProvider, FormField as Field } from '@/components/ui/form'
import { Input } from '@/components/ui/input'
import { Textarea } from '@/components/ui/textarea'
import { Select } from '@/components/ui/select'
import { RadioGroup } from '@/components/ui/radio-group'
import { Checkbox } from '@/components/ui/checkbox'
import { Switch } from '@/components/ui/switch'

interface Props {
  defaultValues?: FieldValues
  children?: ReactNode
  onSubmit?: (values: FieldValues) => void
  className?: string
  disabled?: boolean
}

export function Form({ defaultValues = {}, children, onSubmit, className, disabled }: Props) {
  const methods = useForm({ defaultValues, disabled })
  return <FormProvider {...methods}><form className={className} onSubmit={methods.handleSubmit((values) => onSubmit?.(values))}>{children}</form></FormProvider>
}

function bindFields(children: ReactNode, field: ControllerRenderProps): ReactNode {
  const items = Children.map(children, (child) => {
    if (!isValidElement(child)) return child
    const element = child as ReactElement<Record<string, unknown>>
    let props: Record<string, unknown> = {}
    if (element.type === Input || element.type === Textarea) props = { ...field, value: field.value ?? '' }
    if (element.type === Select || element.type === RadioGroup) props = { value: field.value ?? '', onValueChange: field.onChange, disabled: field.disabled }
    if (element.type === Checkbox || element.type === Switch) props = { checked: Boolean(field.value), onCheckedChange: field.onChange, disabled: field.disabled }
    if (element.props.children) props.children = bindFields(element.props.children as ReactNode, field)
    return cloneElement(element, props)
  })
  return items?.length === 1 ? items[0] : items
}

export function FormField({ name, rules, children }: { name: string; rules?: RegisterOptions; children?: ReactNode }) {
  return <Field name={name} rules={rules} render={({ field }) => <>{bindFields(children, field)}</>} />
}
