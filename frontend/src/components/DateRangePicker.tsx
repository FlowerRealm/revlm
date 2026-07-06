import { useState, useRef, useEffect, useMemo } from 'react';

import { buildDateInputRangePresets } from '../utils/dateInput';

export interface DateRange {
  start: string;
  end: string;
  label?: string;
}

interface DateRangePickerProps {
  start: string;
  end: string;
  onChange: (range: DateRange) => void;
  loading?: boolean;
}

const theme = {
  primary: '#326c52',
  primaryLight: '#f0f7f2',
  primaryBorder: '#d3e4d9',
  textMain: '#212529',
  textMuted: '#6c757d',
  bgHover: '#f8f9fa',
  fontFamily: 'inherit',
};

export function DateRangePicker({ start, end, onChange, loading }: DateRangePickerProps) {
  const [isOpen, setIsOpen] = useState(false);

  // 临时状态仅在面板打开时有意义
  const [tempStart, setTempStart] = useState(start);
  const [tempEnd, setTempEnd] = useState(end);

  const triggerRef = useRef<HTMLDivElement>(null);
  const popoverRef = useRef<HTMLDivElement>(null);
  const startInputRef = useRef<HTMLInputElement>(null);

  const ranges = useMemo(
    () =>
      buildDateInputRangePresets().map((range) => ({
        label: range.label,
        get: () => ({ start: range.start, end: range.end }),
      })),
    []
  );

  // 触发器显示的 Label (基于外部 Props)
  const activeLabel = useMemo(() => {
    const match = ranges.find((r) => {
      const vals = r.get();
      return vals.start === start && vals.end === end;
    });
    if (match) return match.label;
    if (!start && !end) return '全部时间';
    return '自定义';
  }, [start, end, ranges]);

  // 面板内显示的高亮 Label (基于 Temp 状态)
  const tempActiveLabel = useMemo(() => {
    const match = ranges.find((r) => {
      const vals = r.get();
      return vals.start === tempStart && vals.end === tempEnd;
    });
    if (match) return match.label;
    if (!tempStart && !tempEnd) return '全部时间';
    return '自定义';
  }, [tempStart, tempEnd, ranges]);

  const handleToggle = () => {
    if (!isOpen) {
      // 打开时同步当前值为临时值
      setTempStart(start);
      setTempEnd(end);
    }
    setIsOpen(!isOpen);
  };

  const handleRangeSelect = (r: { label: string; get: () => { start: string; end: string } }) => {
    const vals = r.get();
    setTempStart(vals.start);
    setTempEnd(vals.end);
  };

  const handleConfirm = () => {
    onChange({ start: tempStart, end: tempEnd });
    setIsOpen(false);
  };

  useEffect(() => {
    function handleClickOutside(event: MouseEvent) {
      if (
        triggerRef.current &&
        !triggerRef.current.contains(event.target as Node) &&
        popoverRef.current &&
        !popoverRef.current.contains(event.target as Node)
      ) {
        setIsOpen(false);
      }
    }
    document.addEventListener('mousedown', handleClickOutside);
    return () => document.removeEventListener('mousedown', handleClickOutside);
  }, []);

  const styles = {
    wrapper: { position: 'relative' as const, display: 'inline-flex' },
    trigger: {
      display: 'flex',
      alignItems: 'center',
      gap: '4px',
      padding: '4px 8px',
      backgroundColor: 'transparent',
      borderRadius: '4px',
      cursor: 'pointer',
      fontSize: '13.5px',
      color: isOpen ? theme.primary : theme.textMain,
      transition: 'all 0.15s ease-in-out',
      userSelect: 'none' as const,
      whiteSpace: 'nowrap' as const,
      height: '28px',
    },
    icon: {
      fontSize: '16px',
      color: isOpen ? theme.primary : theme.textMuted,
      display: 'flex',
      alignItems: 'center',
    },
    popover: {
      position: 'absolute' as const,
      top: 'calc(100% + 6px)',
      left: '0',
      backgroundColor: '#ffffff',
      borderRadius: '6px',
      boxShadow: '0 8px 30px rgba(0, 0, 0, 0.12)',
      display: isOpen ? 'flex' : 'none',
      overflow: 'hidden',
      zIndex: 1050,
      border: '1px solid #edf2f0',
    },
    sidebar: {
      width: '90px',
      backgroundColor: '#fbfbfc',
      padding: '4px',
      display: 'flex',
      flexDirection: 'column' as const,
      gap: '1px',
      borderRight: '1px solid #f1f3f5',
    },
    rangeBtn: (isActive: boolean) => ({
      padding: '6px 10px',
      backgroundColor: isActive ? theme.primaryLight : 'transparent',
      color: isActive ? theme.primary : theme.textMain,
      border: 'none',
      borderRadius: '3px',
      fontSize: '12px',
      textAlign: 'left' as const,
      cursor: 'pointer',
      fontWeight: isActive ? 500 : 400,
      transition: 'background 0.1s',
      outline: 'none',
    }),
    mainArea: {
      padding: '12px 16px',
      display: 'flex',
      flexDirection: 'column' as const,
      gap: '10px',
      minWidth: '180px',
    },
    inputGroup: { display: 'flex', flexDirection: 'column' as const, gap: '3px' },
    dateLabel: { fontSize: '11px', fontWeight: 500, color: theme.textMuted },
    dateInput: {
      border: '1px solid #e9ecef',
      backgroundColor: '#ffffff',
      padding: '4px 8px',
      borderRadius: '4px',
      fontSize: '12.5px',
      color: theme.textMain,
      outline: 'none',
      fontFamily: 'inherit',
      height: '30px',
      width: '100%',
      boxSizing: 'border-box' as const,
    },
    actions: { display: 'flex', justifyContent: 'flex-end', gap: '6px', marginTop: '4px' },
    btnPrimary: {
      padding: '0 12px',
      backgroundColor: theme.primary,
      color: '#ffffff',
      border: 'none',
      borderRadius: '4px',
      fontSize: '12px',
      cursor: 'pointer',
      fontWeight: 500,
      height: '28px',
    },
    btnSecondary: {
      padding: '0 8px',
      backgroundColor: 'transparent',
      color: theme.textMuted,
      border: 'none',
      borderRadius: '4px',
      fontSize: '12px',
      cursor: 'pointer',
      height: '28px',
    },
  };

  return (
    <div style={styles.wrapper}>
      <div
        ref={triggerRef}
        style={styles.trigger}
        onClick={handleToggle}
        onMouseOver={(e) => {
          if (!isOpen) e.currentTarget.style.backgroundColor = theme.bgHover;
        }}
        onMouseOut={(e) => {
          if (!isOpen) e.currentTarget.style.backgroundColor = 'transparent';
        }}
      >
        <div style={styles.icon}>
          <span className="material-symbols-rounded" style={{ fontSize: '16px' }}>
            calendar_today
          </span>
        </div>
        <span style={{ height: '100%', display: 'flex', alignItems: 'center' }}>
          {activeLabel !== '自定义' ? activeLabel : `${start || '开始'} ~ ${end || '结束'}`}
        </span>
        {loading && (
          <div
            className="spinner-border spinner-border-sm text-success ms-1"
            style={{ width: '10px', height: '12px', borderWidth: '1.5px' }}
          />
        )}
      </div>

      <div ref={popoverRef} style={styles.popover} onClick={(e) => e.stopPropagation()}>
        <div style={styles.sidebar}>
          {ranges.map((r) => (
            <button
              key={r.label}
              style={styles.rangeBtn(tempActiveLabel === r.label)}
              onClick={() => handleRangeSelect(r)}
            >
              {r.label}
            </button>
          ))}
          <button
            style={{ ...styles.rangeBtn(tempActiveLabel === '自定义'), marginTop: '2px' }}
            onClick={() => startInputRef.current?.focus()}
          >
            自定义
          </button>
        </div>

        <div style={styles.mainArea}>
          <div style={styles.inputGroup}>
            <label style={styles.dateLabel}>开始日期</label>
            <input
              ref={startInputRef}
              type="date"
              style={styles.dateInput}
              value={tempStart}
              onChange={(e) => setTempStart(e.target.value)}
              onFocus={(e) => (e.target.style.borderColor = theme.primary)}
              onBlur={(e) => (e.target.style.borderColor = '#e9ecef')}
            />
          </div>
          <div style={styles.inputGroup}>
            <label style={styles.dateLabel}>结束日期</label>
            <input
              type="date"
              style={styles.dateInput}
              value={tempEnd}
              onChange={(e) => setTempEnd(e.target.value)}
              onFocus={(e) => (e.target.style.borderColor = theme.primary)}
              onBlur={(e) => (e.target.style.borderColor = '#e9ecef')}
            />
          </div>

          <div style={styles.actions}>
            <button
              style={styles.btnSecondary}
              onClick={() => {
                setTempStart('');
                setTempEnd('');
              }}
            >
              重置
            </button>
            <button
              style={styles.btnPrimary}
              onClick={handleConfirm}
              onMouseOver={(e) => (e.currentTarget.style.opacity = '0.9')}
              onMouseOut={(e) => (e.currentTarget.style.opacity = '1')}
            >
              应用
            </button>
          </div>
        </div>
      </div>
    </div>
  );
}

interface SelectPickerProps<T> {
  value: T;
  options: Array<{ label: string | number; value: T }>;
  onChange: (value: T) => void;
  label?: string;
}

export function SelectPicker<T extends string | number>({ value, options, onChange, label }: SelectPickerProps<T>) {
  const [isOpen, setIsOpen] = useState(false);
  const triggerRef = useRef<HTMLDivElement>(null);
  const popoverRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    function handleClickOutside(event: MouseEvent) {
      if (
        triggerRef.current &&
        !triggerRef.current.contains(event.target as Node) &&
        popoverRef.current &&
        !popoverRef.current.contains(event.target as Node)
      ) {
        setIsOpen(false);
      }
    }
    document.addEventListener('mousedown', handleClickOutside);
    return () => document.removeEventListener('mousedown', handleClickOutside);
  }, []);

  const styles = {
    wrapper: { position: 'relative' as const, display: 'inline-flex' },
    trigger: {
      display: 'flex',
      alignItems: 'center',
      gap: '2px',
      padding: '4px 8px',
      backgroundColor: 'transparent',
      borderRadius: '4px',
      cursor: 'pointer',
      fontSize: '13.5px',
      color: isOpen ? theme.primary : theme.textMain,
      transition: 'all 0.15s ease-in-out',
      height: '28px',
    },
    icon: {
      fontSize: '16px',
      color: isOpen ? theme.primary : theme.textMuted,
      display: 'flex',
      alignItems: 'center',
    },
    popover: {
      position: 'absolute' as const,
      top: 'calc(100% + 4px)',
      left: '0',
      backgroundColor: '#ffffff',
      borderRadius: '6px',
      boxShadow: '0 8px 30px rgba(0, 0, 0, 0.12)',
      display: isOpen ? 'flex' : 'none',
      flexDirection: 'column' as const,
      padding: '2px',
      zIndex: 1050,
      minWidth: '80px',
      border: '1px solid #edf2f0',
    },
    option: (isActive: boolean) => ({
      padding: '6px 12px',
      backgroundColor: isActive ? theme.primaryLight : 'transparent',
      color: isActive ? theme.primary : theme.textMain,
      border: 'none',
      borderRadius: '3px',
      fontSize: '12.5px',
      textAlign: 'left' as const,
      cursor: 'pointer',
      transition: 'background 0.1s',
      outline: 'none',
    }),
  };

  const selectedLabel = options.find((o) => o.value === value)?.label ?? value;

  return (
    <div style={styles.wrapper}>
      <div
        ref={triggerRef}
        style={styles.trigger}
        onClick={() => setIsOpen(!isOpen)}
        onMouseOver={(e) => {
          if (!isOpen) e.currentTarget.style.backgroundColor = theme.bgHover;
        }}
        onMouseOut={(e) => {
          if (!isOpen) e.currentTarget.style.backgroundColor = 'transparent';
        }}
      >
        <span style={{ display: 'flex', alignItems: 'center', height: '100%' }}>
          {selectedLabel} {label}
        </span>
        <div style={styles.icon}>
          <span className="material-symbols-rounded" style={{ fontSize: '16px' }}>
            {isOpen ? 'expand_less' : 'expand_more'}
          </span>
        </div>
      </div>

      <div ref={popoverRef} style={styles.popover} onClick={(e) => e.stopPropagation()}>
        {options.map((opt) => (
          <button
            key={opt.value}
            style={styles.option(value === opt.value)}
            onClick={() => {
              onChange(opt.value);
              setIsOpen(false);
            }}
          >
            {opt.label}
          </button>
        ))}
      </div>
    </div>
  );
}
