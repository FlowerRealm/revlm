import type { RefObject } from 'react';

import { DateRangePicker, SelectPicker } from '../../../components/DateRangePicker';
import type { UsageAdvancedFiltersDropdownHandle } from '../../../components/UsageAdvancedFiltersDropdown';
import { UsageAdminAdvancedFilters } from './UsageAdminAdvancedFilters';

type Props = {
  advRef: RefObject<UsageAdvancedFiltersDropdownHandle | null>;
  start: string;
  end: string;
  loading: boolean;
  limit: number;
  filterUser: string;
  filterChannel: string;
  filterModel: string;
  onDateRangeChange: (range: { start: string; end: string }) => void;
  onLimitChange: (value: number) => void;
  onUserChange: (value: string) => void;
  onChannelChange: (value: string) => void;
  onModelChange: (value: string) => void;
  onRefresh: () => void;
  onReset: () => void;
};

export function UsageAdminFilterBar({
  advRef,
  start,
  end,
  loading,
  limit,
  filterUser,
  filterChannel,
  filterModel,
  onDateRangeChange,
  onLimitChange,
  onUserChange,
  onChannelChange,
  onModelChange,
  onRefresh,
  onReset,
}: Props) {
  return (
    <div className="card border-0 shadow-sm mb-0">
      <div className="card-body py-3 px-4">
        <div className="d-flex flex-wrap align-items-end gap-3">
          <div className="d-flex flex-wrap align-items-center gap-2">
            <div className="text-muted smaller fw-medium text-nowrap">时间区间</div>
            <DateRangePicker start={start} end={end} onChange={onDateRangeChange} loading={loading} />
          </div>

          <div className="d-flex flex-wrap align-items-center gap-2">
            <div className="text-muted smaller fw-medium text-nowrap">显示条数</div>
            <SelectPicker
              value={limit}
              options={[
                { label: '20', value: 20 },
                { label: '50', value: 50 },
                { label: '100', value: 100 },
                { label: '200', value: 200 },
              ]}
              label="条"
              onChange={(value) => onLimitChange(Number(value))}
            />
          </div>

          <div className="d-flex align-items-center gap-2">
            <UsageAdminAdvancedFilters
              advRef={advRef}
              disabled={loading}
              filterUser={filterUser}
              filterChannel={filterChannel}
              filterModel={filterModel}
              onUserChange={onUserChange}
              onChannelChange={onChannelChange}
              onModelChange={onModelChange}
            />
          </div>

          <div className="ms-auto d-flex gap-2">
            <button className="btn btn-primary btn-sm" type="button" disabled={loading} onClick={onRefresh}>
              <span className="material-symbols-rounded me-1">refresh</span>
              更新
            </button>
            <button className="btn btn-light border btn-sm" type="button" disabled={loading} onClick={onReset}>
              重置
            </button>
          </div>
        </div>
      </div>
    </div>
  );
}
