// 面板没有引第三方类名合并库，这里只做最必要的拼接
export function cn(...values: (string | false | null | undefined)[]): string {
  return values.filter(Boolean).join(' ')
}
