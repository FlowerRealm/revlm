import { useState } from 'react';
import type { Meta, StoryObj } from '@storybook/react-vite';

import { UsageEventsCard } from './UsageEventsCard';
import type { UsageEventDetail } from '../../api/usage';
import { tokenByID, usageEventDetail, usageEvents } from '../../storybook/fixtures/usage';

/** Click a row to expand it and lazily "load" its `usage_details` (mocked, no network). */
function Demo({ loading, events = usageEvents }: { loading?: boolean; events?: typeof usageEvents }) {
  const [expandedID, setExpandedID] = useState<number | null>(null);
  const [detailLoadingID, setDetailLoadingID] = useState<number | null>(null);
  const [detailByEventID, setDetailByEventID] = useState<Record<number, UsageEventDetail>>({});

  function loadDetail(eventID: number) {
    if (detailByEventID[eventID]) return;
    setDetailLoadingID(eventID);
    window.setTimeout(() => {
      setDetailByEventID((prev) => ({ ...prev, [eventID]: { ...usageEventDetail, event_id: eventID } }));
      setDetailLoadingID(null);
    }, 400);
  }

  return (
    <UsageEventsCard
      events={events}
      tokenByID={tokenByID}
      expandedID={expandedID}
      setExpandedID={setExpandedID}
      loadDetail={loadDetail}
      detailLoadingID={detailLoadingID}
      detailByEventID={detailByEventID}
      canPrev={false}
      canNext={events.length > 0}
      loading={!!loading}
      onPrevPage={() => {}}
      onNextPage={() => {}}
      selfEmail="alice@example.com"
      selfID={1}
    />
  );
}

const meta: Meta = {
  title: 'Pages/Usage/UsageEventsCard',
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
