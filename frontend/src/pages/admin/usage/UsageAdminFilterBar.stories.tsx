import { useRef, useState } from 'react';
import type { Meta, StoryObj } from '@storybook/react-vite';

import { UsageAdminFilterBar } from './UsageAdminFilterBar';
import type { UsageAdvancedFiltersDropdownHandle } from '../../../components/UsageAdvancedFiltersDropdown';
import { adminUsageWindow } from '../../../storybook/fixtures/adminUsage';

function Demo({ loading }: { loading?: boolean }) {
  const advRef = useRef<UsageAdvancedFiltersDropdownHandle | null>(null);
  const [range, setRange] = useState({ start: '2026-08-12', end: adminUsageWindow.until.slice(0, 10) });
  const [limit, setLimit] = useState(50);
  const [filterUser, setFilterUser] = useState('');
  const [filterChannel, setFilterChannel] = useState('');
  const [filterModel, setFilterModel] = useState('');

  return (
    <UsageAdminFilterBar
      advRef={advRef}
      start={range.start}
      end={range.end}
      loading={!!loading}
      limit={limit}
      filterUser={filterUser}
      filterChannel={filterChannel}
      filterModel={filterModel}
      onDateRangeChange={setRange}
      onLimitChange={setLimit}
      onUserChange={setFilterUser}
      onChannelChange={setFilterChannel}
      onModelChange={setFilterModel}
      onRefresh={() => {}}
      onReset={() => {
        setFilterUser('');
        setFilterChannel('');
        setFilterModel('');
      }}
    />
  );
}

const meta: Meta = {
  title: 'Pages/Admin/Usage/UsageAdminFilterBar',
  parameters: { layout: 'padded' },
};

export default meta;

export const Default: StoryObj = {
  render: () => <Demo />,
};

export const Loading: StoryObj = {
  render: () => <Demo loading />,
};
