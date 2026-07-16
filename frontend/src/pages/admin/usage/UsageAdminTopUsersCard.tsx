import type { AdminUsageUser } from '../../../api/admin/usage';

export function UsageAdminTopUsersCard({ topUsers }: { topUsers: AdminUsageUser[] }) {
  return (
    <div className="card border-0 p-0 overflow-hidden">
      <div className="card-header bg-white py-3 border-bottom-0 px-4">
        <h5 className="mb-0 fw-bold">
          <i className="ri-group-line me-2"></i>
          消费排行用户（统计区间）
        </h5>
      </div>
      <div className="card-body p-0">
        <div className="table-responsive">
          <table className="table table-hover align-middle mb-0 border-0">
            <thead className="table-light text-muted smaller uppercase">
              <tr>
                <th className="ps-4 border-0">用户</th>
                <th className="border-0">状态</th>
                <th className="text-end pe-4 border-0">已结算费用</th>
              </tr>
            </thead>
            <tbody>
              {topUsers.map((user) => (
                <tr key={user.user_id}>
                  <td className="ps-4">
                    <div className="d-flex align-items-center">
                      <div
                        className="bg-primary bg-opacity-10 text-primary rounded-circle d-flex align-items-center justify-content-center me-3"
                        style={{ width: 32, height: 32 }}
                      >
                        {(user.email || '?').slice(0, 1)}
                      </div>
                      <div>
                        <div className="fw-bold small">{user.email}</div>
                        <div className="text-muted smaller">{user.role}</div>
                      </div>
                    </div>
                  </td>
                  <td>
                    {user.status === 1 ? (
                      <span className="badge bg-success-subtle text-success border border-success-subtle rounded-pill px-2">
                        正常
                      </span>
                    ) : (
                      <span className="badge bg-danger-subtle text-danger border border-danger-subtle rounded-pill px-2">
                        禁用
                      </span>
                    )}
                  </td>
                  <td className="text-end font-monospace small fw-bold text-dark pe-4">{user.usd}</td>
                </tr>
              ))}
              {topUsers.length === 0 ? (
                <tr>
                  <td colSpan={3} className="text-center py-5 text-muted small">
                    暂无用户用量数据
                  </td>
                </tr>
              ) : null}
            </tbody>
          </table>
        </div>
      </div>
    </div>
  );
}
