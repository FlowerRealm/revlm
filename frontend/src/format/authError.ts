export type PageError = {
  summary: string;
  detail?: string;
};

function truncateText(s: string, maxLen: number): string {
  const v = (s || '').toString();
  if (v.length <= maxLen) return v;
  return v.slice(0, Math.max(0, maxLen - 1)) + '…';
}

function toErrorMessage(err: unknown): string {
  if (typeof err === 'string') return err;
  if (err && typeof err === 'object') {
    const anyErr = err as { message?: unknown; response?: { data?: unknown } };
    const resp = anyErr.response?.data as { message?: unknown } | undefined;
    const respMsg = typeof resp?.message === 'string' ? resp.message : '';
    if (respMsg.trim()) return respMsg.trim();
    const msg = typeof anyErr.message === 'string' ? anyErr.message : '';
    if (msg.trim()) return msg.trim();
  }
  if (err instanceof Error) return err.message || '';
  return '';
}

export function formatAuthError(action: '登录' | '注册', err: unknown): PageError {
  const actionFailed = `${action}失败`;
  const raw = (toErrorMessage(err) || '').toString().trim() || actionFailed;
  const compact = raw.toLowerCase();

  let summary = raw;

  if (
    compact.includes('network error') ||
    compact.includes('request failed with status code') ||
    compact.includes('timeout')
  ) {
    summary = '网络异常或服务不可用，请稍后重试';
  } else if (action === '登录') {
    if (raw.includes('邮箱/账号名或密码错误')) summary = '登录失败：账号或密码不正确';
    else if (raw.includes('无效的参数')) summary = '请输入正确的账号与密码';
    else if (!raw.includes('登录') && !raw.includes('失败') && !raw.includes('错误')) summary = `登录失败：${raw}`;
  } else {
    if (raw.includes('账号名已被占用')) summary = '注册失败：账号名已被占用';
    else if (raw.includes('邮箱或密码不能为空')) summary = '注册失败：邮箱或密码不能为空';
    else if (raw.includes('无效的参数')) summary = '注册失败：请检查填写内容';
    else if (raw.includes('创建用户失败')) summary = '注册失败：创建账号失败，请稍后重试';
    else if (!raw.includes('注册') && !raw.includes('失败') && !raw.includes('错误')) summary = `注册失败：${raw}`;
  }

  const detail = summary !== raw ? truncateText(raw, 400) : undefined;
  return { summary, detail };
}
