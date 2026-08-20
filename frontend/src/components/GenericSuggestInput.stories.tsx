import { useState } from 'react';
import type { Meta, StoryObj } from '@storybook/react-vite';

import { GenericSuggestInput } from './GenericSuggestInput';

type Suggestion = { id: number; email: string };

const allUsers: Suggestion[] = [
  { id: 1, email: 'alice@example.com' },
  { id: 2, email: 'bob@example.com' },
  { id: 3, email: 'carol@example.com' },
  { id: 4, email: 'dave@example.com' },
];

async function fetchUsers(q: string): Promise<Suggestion[]> {
  await new Promise((resolve) => window.setTimeout(resolve, 250));
  const needle = q.trim().toLowerCase();
  return allUsers.filter((u) => u.email.toLowerCase().includes(needle));
}

/** Type "a" or "example" to see the suggestion panel — it debounces `fetchItems` by 200ms. */
function Demo() {
  const [value, setValue] = useState('');
  const [selected, setSelected] = useState<Suggestion | null>(null);
  return (
    <div style={{ width: 320 }}>
      <GenericSuggestInput<Suggestion>
        id="storybook-suggest-input"
        value={value}
        placeholder="按邮箱搜索用户…"
        onChange={setValue}
        onSelect={(item) => {
          setSelected(item);
          setValue(item.email);
        }}
        fetchItems={fetchUsers}
        getItemKey={(item) => item.id}
        renderItem={(item) => item.email}
        emptyText="无匹配用户"
      />
      <div className="text-muted small mt-2">已选中：{selected ? `#${selected.id} ${selected.email}` : '（无）'}</div>
    </div>
  );
}

const meta = {
  title: 'Components/GenericSuggestInput',
  component: GenericSuggestInput<Suggestion>,
  parameters: { layout: 'centered' },
} satisfies Meta<typeof GenericSuggestInput<Suggestion>>;

export default meta;
type Story = StoryObj<typeof meta>;

export const SearchAndSelect: Story = {
  args: {
    id: 'storybook-suggest-input',
    value: '',
    onChange: () => {},
    onSelect: () => {},
    fetchItems: fetchUsers,
    getItemKey: (item) => item.id,
    renderItem: (item) => item.email,
    emptyText: '无匹配用户',
  },
  render: () => <Demo />,
};
