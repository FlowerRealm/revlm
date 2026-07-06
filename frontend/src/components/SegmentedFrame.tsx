import { Children, type ReactNode } from 'react';

export function SegmentedFrame({
  children,
  className,
  segmentClassName,
}: {
  children: ReactNode;
  className?: string;
  segmentClassName?: string;
}) {
  const parts = Children.toArray(children).filter((c) => c !== null && c !== undefined);
  return (
    <div className={['rlm-segmented', className].filter(Boolean).join(' ')}>
      {parts.map((child, i) => (
        <div key={i} className={['rlm-segment', segmentClassName].filter(Boolean).join(' ')}>
          {child}
        </div>
      ))}
    </div>
  );
}
