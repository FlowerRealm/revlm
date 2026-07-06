import { Children, type ReactNode } from 'react';

export function DividedStack({
  children,
  className,
  itemClassName,
}: {
  children: ReactNode;
  className?: string;
  itemClassName?: string;
}) {
  const parts = Children.toArray(children).filter((c) => {
    if (c === null || c === undefined) return false;
    if (typeof c === 'string' && c.trim() === '') return false;
    return true;
  });
  return (
    <div className={['rlm-divided-stack', className].filter(Boolean).join(' ')}>
      {parts.map((child, i) => (
        <div key={i} className={['rlm-divided-item', itemClassName].filter(Boolean).join(' ')}>
          {child}
        </div>
      ))}
    </div>
  );
}
