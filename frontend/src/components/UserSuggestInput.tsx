import { getAdminUsageUserSuggest, type AdminUsageUserSuggest } from '../api/admin/usage';
import { GenericSuggestInput } from './GenericSuggestInput';

export function UserSuggestInput(props: {
  id: string;
  value: string;
  disabled?: boolean;
  placeholder?: string;
  onChange: (value: string) => void;
  onSelect: (u: AdminUsageUserSuggest) => void;
}) {
  const { id, value, disabled, placeholder, onChange, onSelect } = props;

  return (
    <GenericSuggestInput<AdminUsageUserSuggest>
      id={id}
      value={value}
      placeholder={placeholder}
      disabled={disabled}
      minWidth={360}
      maxWidth={520}
      emptyText="无匹配用户"
      onChange={onChange}
      onSelect={onSelect}
      fetchItems={async (q) => {
        const res = await getAdminUsageUserSuggest(q, 20);
        if (!res.success) throw new Error(res.message || '加载失败');
        return res.data || [];
      }}
      getItemKey={(u) => u.id}
      renderItem={(u) => (
        <>
          <div className="d-flex justify-content-between align-items-center">
            <div className="small fw-semibold text-truncate">{u.email}</div>
            <div className="text-muted smaller ms-2">ID: {u.id}</div>
          </div>
          <div className="text-muted smaller text-truncate">@{u.username}</div>
        </>
      )}
    />
  );
}
