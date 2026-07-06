import { getAdminUsageChannelSuggest, type AdminUsageChannelSuggest } from '../api/admin/usage';
import { GenericSuggestInput } from './GenericSuggestInput';

export function ChannelSuggestInput(props: {
  id: string;
  value: string;
  disabled?: boolean;
  placeholder?: string;
  start?: string;
  end?: string;
  allTime?: boolean;
  onChange: (value: string) => void;
  onSelect: (ch: AdminUsageChannelSuggest) => void;
}) {
  const { id, value, disabled, placeholder, start, end, allTime, onChange, onSelect } = props;

  return (
    <GenericSuggestInput<AdminUsageChannelSuggest>
      id={id}
      value={value}
      placeholder={placeholder}
      disabled={disabled}
      minWidth={380}
      maxWidth={560}
      emptyText="无匹配渠道"
      onChange={onChange}
      onSelect={onSelect}
      fetchItems={async (q) => {
        const res = await getAdminUsageChannelSuggest({
          q,
          limit: 20,
          start: start || undefined,
          end: end || undefined,
          all_time: !!allTime || undefined,
        });
        if (!res.success) throw new Error(res.message || '加载失败');
        return res.data || [];
      }}
      getItemKey={(ch) => ch.id}
      renderItem={(ch) => (
        <>
          <div className="d-flex justify-content-between align-items-center">
            <div className="small fw-semibold text-truncate">{ch.name}</div>
            <div className="text-muted smaller ms-2">ID: {ch.id}</div>
          </div>
          <div className="text-muted smaller text-truncate">{ch.type}</div>
        </>
      )}
    />
  );
}
