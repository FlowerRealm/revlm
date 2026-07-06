import { getAdminUsageModelSuggest, type AdminUsageModelSuggest } from '../api/admin/usage';
import { GenericSuggestInput } from './GenericSuggestInput';

export function ModelSuggestInput(props: {
  id: string;
  value: string;
  disabled?: boolean;
  placeholder?: string;
  start?: string;
  end?: string;
  allTime?: boolean;
  onChange: (value: string) => void;
  onSelect: (m: string) => void;
}) {
  const { id, value, disabled, placeholder, start, end, allTime, onChange, onSelect } = props;

  return (
    <GenericSuggestInput<AdminUsageModelSuggest>
      id={id}
      value={value}
      placeholder={placeholder}
      disabled={disabled}
      minWidth={380}
      maxWidth={560}
      emptyText="无匹配模型"
      onChange={onChange}
      onSelect={(it) => onSelect(it.model)}
      fetchItems={async (q) => {
        const res = await getAdminUsageModelSuggest({
          q,
          limit: 20,
          start: start || undefined,
          end: end || undefined,
          all_time: !!allTime || undefined,
        });
        if (!res.success) throw new Error(res.message || '加载失败');
        return res.data || [];
      }}
      getItemKey={(it) => it.model}
      renderItem={(it) => <div className="small fw-semibold text-truncate">{it.model}</div>}
    />
  );
}
