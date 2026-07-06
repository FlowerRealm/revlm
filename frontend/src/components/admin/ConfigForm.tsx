import { useEffect, useMemo, useState, type FormEvent, type ReactNode } from 'react';

import type { ConfigChoice, ConfigField, ConfigValue, ConfigValues } from './configTemplates';

type ConfigFormProps<TValues extends ConfigValues, TResult> = {
  template: {
    defaults: TValues;
    fields(values: TValues): readonly ConfigField<TValues>[];
    save(values: TValues): Promise<TResult>;
    submit: string;
  };
  initialValues?: Partial<TValues> | null;
  layout?: 'grid' | 'stack';
  fieldClassName?: string;
  submitClassName?: string;
  submitDisabled?: boolean;
  footerStart?: ReactNode;
  onError?: (message: string) => void;
  onResult?: (result: TResult) => Promise<unknown> | unknown;
  onSaved?: () => Promise<unknown> | unknown;
  onSubmitStart?: () => void;
  resetOnSaved?: boolean;
};

function errorMessage(err: unknown, fallback = '操作失败') {
  return err instanceof Error ? err.message : fallback;
}

function isBlank(value: ConfigValue) {
  return value == null || String(value).trim() === '';
}

function choiceValue(choice: ConfigChoice<ConfigValue>) {
  return typeof choice === 'object' && choice !== null ? choice.value : choice;
}

function choiceLabel(choice: ConfigChoice<ConfigValue>) {
  return typeof choice === 'object' && choice !== null ? choice.label : String(choice);
}

function parseControlValue<TValues extends ConfigValues>(field: ConfigField<TValues>, raw: string, values: TValues) {
  if (field.parse) return field.parse(raw, values);
  const current = values[field.key];
  if (typeof current === 'number') {
    return raw.trim() === '' ? null : Number(raw);
  }
  if (typeof current === 'boolean') {
    return raw === 'true';
  }
  return raw;
}

function control<TValues extends ConfigValues>(
  field: ConfigField<TValues>,
  values: TValues,
  setValue: (field: ConfigField<TValues>, value: string) => void
) {
  const { key, type = 'text', rows, choices } = field;
  const value = values[key] ?? '';
  if (choices) {
    return (
      <select className="form-select" value={String(value)} onChange={(e) => setValue(field, e.target.value)}>
        {choices.map((choice) => (
          <option key={String(choiceValue(choice))} value={String(choiceValue(choice))}>
            {choiceLabel(choice)}
          </option>
        ))}
      </select>
    );
  }
  if (type === 'textarea') {
    return (
      <textarea
        className="form-control"
        rows={rows || 3}
        value={String(value)}
        onChange={(e) => setValue(field, e.target.value)}
      />
    );
  }
  return (
    <input
      className="form-control"
      type={type}
      value={String(value)}
      onChange={(e) => setValue(field, e.target.value)}
    />
  );
}

export function ConfigForm<TValues extends ConfigValues, TResult>({
  template,
  initialValues,
  layout = 'grid',
  fieldClassName,
  submitClassName = 'btn btn-primary px-4',
  submitDisabled,
  footerStart,
  onError,
  onResult,
  onSaved,
  onSubmitStart,
  resetOnSaved,
}: ConfigFormProps<TValues, TResult>) {
  const initial = useMemo(() => ({ ...template.defaults, ...initialValues }) as TValues, [template, initialValues]);
  const [values, setValues] = useState<TValues>(initial);
  const [saving, setSaving] = useState(false);
  const fields = template.fields(values);
  const validationError = fields
    .map((field) => {
      const value = values[field.key];
      if (field.required && isBlank(value)) return `${field.label} 不能为空`;
      return field.validate?.(value, values) || '';
    })
    .find(Boolean);
  const canSubmit = !validationError;

  useEffect(() => {
    setValues(initial);
  }, [initial]);

  function setValue(field: ConfigField<TValues>, value: string) {
    setValues((prev) => ({ ...prev, [field.key]: parseControlValue(field, value, prev) }));
  }

  async function submit(e: FormEvent<HTMLFormElement>) {
    e.preventDefault();
    onSubmitStart?.();
    setSaving(true);
    try {
      const result = await template.save(values);
      await onResult?.(result);
      await onSaved?.();
      if (resetOnSaved ?? !initialValues) setValues(template.defaults);
    } catch (err) {
      onError?.(errorMessage(err));
    } finally {
      setSaving(false);
    }
  }

  const resolvedFieldClassName = fieldClassName || (layout === 'grid' ? 'col-md-6' : 'mb-3');
  return (
    <form className={layout === 'grid' ? 'row g-3' : undefined} onSubmit={(e) => void submit(e)}>
      {fields.map((field) => {
        return (
          <div key={field.key} className={resolvedFieldClassName}>
            <label className="form-label">{field.label}</label>
            {control(field, values, setValue)}
            {field.hint ? <div className="form-text small text-muted">{field.hint}</div> : null}
          </div>
        );
      })}
      <div className={layout === 'grid' ? 'modal-footer border-top-0 px-0 pb-0' : undefined}>
        {footerStart}
        <button
          className={submitClassName}
          type="submit"
          disabled={saving || submitDisabled || !canSubmit}
          title={validationError || undefined}
        >
          {saving ? '保存中…' : template.submit}
        </button>
      </div>
    </form>
  );
}
