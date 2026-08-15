import type { AdminUsageEvent } from '../../../api/admin/usage';

export function UsageAdminEventStatusCell({ event }: { event: AdminUsageEvent }) {
  return (
    <td className="text-center text-nowrap">
      {event.error ? (
        <div className="text-danger smaller" title={event.error}>
          <span className="material-symbols-rounded">error</span> 错误
        </div>
      ) : (
        <span className="text-muted">-</span>
      )}
    </td>
  );
}
