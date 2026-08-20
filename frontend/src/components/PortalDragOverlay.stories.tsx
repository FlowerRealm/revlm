import { useState } from 'react';
import { DndContext, useDraggable, type DragEndEvent } from '@dnd-kit/core';
import type { Meta, StoryObj } from '@storybook/react-vite';

import { PortalDragOverlay } from './PortalDragOverlay';

function DraggableChip({ label }: { label: string }) {
  const { attributes, listeners, setNodeRef, transform } = useDraggable({ id: label });
  return (
    <button
      ref={setNodeRef}
      {...listeners}
      {...attributes}
      type="button"
      className="btn btn-outline-primary"
      style={{ transform: transform ? `translate(${transform.x}px, ${transform.y}px)` : undefined }}
    >
      {label}
    </button>
  );
}

/** Drag the chip below — `PortalDragOverlay` mounts dnd-kit's `DragOverlay` onto `document.body`. */
function DragDemo() {
  const [activeId, setActiveId] = useState<string | null>(null);

  function handleDragEnd(event: DragEndEvent) {
    setActiveId(null);
    void event;
  }

  return (
    <DndContext onDragStart={(e) => setActiveId(String(e.active.id))} onDragEnd={handleDragEnd}>
      <div className="d-flex gap-3">
        <DraggableChip label="拖动我" />
      </div>
      <PortalDragOverlay>
        {activeId ? <div className="btn btn-primary shadow">{activeId}</div> : null}
      </PortalDragOverlay>
    </DndContext>
  );
}

const meta = {
  title: 'Components/PortalDragOverlay',
  component: PortalDragOverlay,
  parameters: { layout: 'centered' },
} satisfies Meta<typeof PortalDragOverlay>;

export default meta;
type Story = StoryObj<typeof meta>;

export const DragToSeeOverlay: Story = {
  render: () => <DragDemo />,
};
