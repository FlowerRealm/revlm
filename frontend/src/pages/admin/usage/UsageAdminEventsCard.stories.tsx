import { useState } from 'react';
import type { Meta, StoryObj } from '@storybook/react-vite';

import { UsageAdminEventsCard } from './UsageAdminEventsCard';
import type { UsageEventDetail } from '../../../api/admin/usage';
import { adminUsageEventDetail, adminUsageEvents } from '../../../storybook/fixtures/adminUsage';

/** Click a row to expand it and lazily "load" its `usage_details` (mocked, no network). */
function Demo({ loading, events = adminUsageEvents }: { loading?: boolean; events?: typeof adminUsageEvents }) {
  const [expandedID, setExpandedID] = useState<number | null>(null);
  const [detailLoadingID, setDetailLoadingID] = useState<number | null>(null);
  const [detailByEventID, setDetailByEventID] = useState<Record<number, UsageEventDetail>>({});

  function toggle(eventID: number) {
    const next = expandedID === eventID ? null : eventID;
    setExpandedID(next);
    if (next && !detailByEventID[eventID]) {
      setDetailLoadingID(eventID);
      window.setTimeout(() => {
        setDetailByEventID((prev) => ({ ...prev, [eventID]: { ...adminUsageEventDetail, event_id: eventID } }));
        setDetailLoadingID(null);
      }, 400);
    }
  }

  return (
    <UsageAdminEventsCard
      events={events}
      expandedID={expandedID}
      detailByEventID={detailByEventID}
      detailLoadingID={detailLoadingID}
      canPrev={false}
      canNext={events.length > 0}
      loading={!!loading}
      onToggleEvent={toggle}
      onPrevPage={() => {}}
      onNextPage={() => {}}
    />
  );
}

const meta: Meta = {
  title: 'Pages/Admin/Usage/UsageAdminEventsCard',
  parameters: { layout: 'padded' },
};

export default meta;

export const Default: StoryObj = {
  render: () => <Demo />,
};

export const Empty: StoryObj = {
  render: () => <Demo events={[]} />,
};

export const Loading: StoryObj = {
  render: () => <Demo loading />,
};
