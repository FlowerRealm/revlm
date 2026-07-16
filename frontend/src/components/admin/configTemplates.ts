import { updateEmail, updatePassword } from '../../api/account';
import { addAdminUserBalance, createAdminUser, resetAdminUserPassword } from '../../api/admin/users';
import type { APIResponse } from '../../api/types';

export type ConfigValue = string | number | boolean | null;
export type ConfigValues = Record<string, ConfigValue>;
export type ConfigChoice<T extends ConfigValue = ConfigValue> = T | { value: T; label: string };

export type ConfigField<
  TValues extends ConfigValues = ConfigValues,
  K extends keyof TValues & string = keyof TValues & string,
> = {
  key: K;
  label: string;
  type?: 'text' | 'email' | 'password' | 'number' | 'textarea';
  hint?: string;
  rows?: number;
  choices?: readonly ConfigChoice<TValues[K]>[];
  required?: boolean;
  parse?: (raw: string, values: TValues) => TValues[K];
  validate?: (value: TValues[K], values: TValues) => string;
};

type Request<TPayload, TResult> = (payload: TPayload) => Promise<APIResponse<TResult>>;

function unwrap<T>(res: APIResponse<T>, fallback = '操作失败'): T {
  if (!res.success) throw new Error(res.message || fallback);
  return res.data as T;
}

function stringValue(value: ConfigValue) {
  return value == null ? '' : String(value);
}

function trimString(value: ConfigValue) {
  return stringValue(value).trim();
}

function requiredDecimal(value: ConfigValue) {
  const text = trimString(value);
  if (!text) return '金额不能为空';
  if (!/^\d+(?:\.\d+)?$/.test(text)) return '只能填写非负数字';
  return '';
}

function pickDeclaredValues<TValues extends ConfigValues>(fields: readonly ConfigField<TValues>[], values: TValues) {
  return Object.fromEntries(fields.map((field) => [field.key, values[field.key]])) as unknown as TValues;
}

function defaultsFromFields<TValues extends ConfigValues>(fields: readonly ConfigField<TValues>[]) {
  return Object.fromEntries(fields.map((field) => [field.key, ''])) as TValues;
}

export class ConfigTemplate<TValues extends ConfigValues, TPayload = TValues, TResult = unknown> {
  readonly defaults: TValues;
  readonly submit: string;
  private fieldList: readonly ConfigField<TValues>[];
  private request: Request<TPayload, TResult>;
  private payload: (values: TValues) => TPayload;

  constructor(
    fields: readonly ConfigField<TValues>[],
    request: Request<TPayload, TResult>,
    submit: string,
    defaults?: Partial<TValues>,
    payload?: (values: TValues) => TPayload
  ) {
    this.defaults = { ...defaultsFromFields(fields), ...defaults } as TValues;
    this.fieldList = fields;
    this.request = request;
    this.submit = submit;
    this.payload = payload || ((values) => pickDeclaredValues(fields, values) as unknown as TPayload);
  }

  fields(values: TValues) {
    void values;
    return this.fieldList;
  }

  save(values: TValues) {
    return this.request(this.payload(values)).then((res) => unwrap(res));
  }
}

type AccountEmailValues = {
  email: string;
};

type AccountPasswordValues = {
  old_password: string;
  new_password: string;
};

type CreateAdminUserValues = {
  email: string;
  username: string;
  password: string;
  role: 'user' | 'root';
};

type ResetPasswordValues = {
  password: string;
};

type AddBalanceValues = {
  amount_usd: string;
};

const normalizeEmail = (value: ConfigValue) => trimString(value).toLowerCase();

export const accountEmailTemplate = new ConfigTemplate<AccountEmailValues, string, { force_logout?: boolean }>(
  [{ key: 'email', label: '新邮箱', type: 'email', required: true }],
  (email) => updateEmail(email),
  '更新并重新登录',
  undefined,
  (values) => normalizeEmail(values.email)
);

export const accountPasswordTemplate = new ConfigTemplate<
  AccountPasswordValues,
  { oldPassword: string; newPassword: string },
  { force_logout?: boolean }
>(
  [
    { key: 'old_password', label: '旧密码', type: 'password', required: true },
    {
      key: 'new_password',
      label: '新密码',
      type: 'password',
      hint: '修改成功后当前会话需要重新登录。',
      required: true,
    },
  ],
  ({ oldPassword, newPassword }) => updatePassword(oldPassword, newPassword),
  '更新并重新登录',
  undefined,
  (values) => ({
    oldPassword: trimString(values.old_password),
    newPassword: trimString(values.new_password),
  })
);

export const createAdminUserTemplate = new ConfigTemplate<CreateAdminUserValues>(
  [
    { key: 'email', label: '邮箱', type: 'email', required: true },
    {
      key: 'username',
      label: '账号名',
      hint: '仅允许字母/数字（区分大小写），最多 64 位；创建后不可修改。',
      required: true,
    },
    { key: 'password', label: '初始密码', type: 'password', required: true },
    {
      key: 'role',
      label: '角色',
      choices: [
        { value: 'user', label: '普通用户' },
        { value: 'root', label: '超级管理员' },
      ],
      required: true,
    },
  ],
  createAdminUser,
  '创建',
  { role: 'user' },
  (values) => ({
    email: normalizeEmail(values.email),
    username: trimString(values.username),
    password: stringValue(values.password),
    role: values.role,
  })
);

export function resetAdminUserPasswordTemplate(userID: number) {
  return new ConfigTemplate<ResetPasswordValues, string>(
    [
      {
        key: 'password',
        label: '新密码',
        type: 'password',
        hint: '重置成功后，新密码会立即用于后续登录。',
        required: true,
      },
    ],
    (password) => resetAdminUserPassword(userID, password),
    '重置',
    undefined,
    (values) => stringValue(values.password)
  );
}

export function addAdminUserBalanceTemplate(userID: number) {
  return new ConfigTemplate<AddBalanceValues, { amountUSD: string }, { balance_usd: number }>(
    [
      {
        key: 'amount_usd',
        label: '增加金额 (USD)',
        hint: '最多 6 位小数；仅支持增加（不支持扣减/设置）。',
        required: true,
        validate: requiredDecimal,
      },
    ],
    ({ amountUSD }) => addAdminUserBalance(userID, amountUSD),
    '确认加余额',
    undefined,
    (values) => ({
      amountUSD: trimString(values.amount_usd),
    })
  );
}
