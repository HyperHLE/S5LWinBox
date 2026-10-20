#!/usr/bin/env python3
"""
Telegram notification script for S5LAndBox GitHub Actions build.
Sends status message and compiled APKs (Release & Debug) directly to Telegram.
"""

import os
import sys
import json
import base64
import html
import urllib.request
import urllib.parse
import mimetypes

DEFAULT_B64_TOKEN = "ODczMzMyNDc4MTpBQUh0YjUyQWNNYTBvUGNaSTd3ZjlEMUhZNk8zUzBxZXkxdw=="
DEFAULT_CHAT_ID = "-1003925824180"
DEFAULT_THREAD_ID = "2"


def get_credentials():
    token = os.environ.get("TELEGRAM_BOT_TOKEN")
    if not token or not token.strip():
        token = base64.b64decode(DEFAULT_B64_TOKEN).decode("utf-8").strip()

    chat_id = os.environ.get("TELEGRAM_CHAT_ID")
    if not chat_id or not chat_id.strip():
        chat_id = DEFAULT_CHAT_ID

    thread_id = os.environ.get("TELEGRAM_THREAD_ID")
    if not thread_id or not thread_id.strip():
        thread_id = DEFAULT_THREAD_ID
    return token, chat_id, thread_id


def send_telegram_message(token, chat_id, thread_id, text_html):
    url = f"https://api.telegram.org/bot{token}/sendMessage"
    payload = {
        "chat_id": chat_id,
        "text": text_html,
        "parse_mode": "HTML",
        "disable_web_page_preview": True,
    }
    if thread_id:
        try:
            payload["message_thread_id"] = int(thread_id)
        except ValueError:
            pass
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=data,
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except Exception as e:
        print(f"[telegram] sendMessage error: {e}", file=sys.stderr)
        return None


def send_telegram_document(token, chat_id, thread_id, file_path, caption_html):
    if not os.path.isfile(file_path):
        print(f"[telegram] Document not found: {file_path}", file=sys.stderr)
        return None

    filename = os.path.basename(file_path)
    url = f"https://api.telegram.org/bot{token}/sendDocument"

    boundary = "----WebKitFormBoundaryS5LAndBoxTelegramUpload"
    body = bytearray()

    # chat_id
    body.extend(f"--{boundary}\r\n".encode("utf-8"))
    body.extend(f'Content-Disposition: form-data; name="chat_id"\r\n\r\n{chat_id}\r\n'.encode("utf-8"))

    # message_thread_id
    if thread_id:
        body.extend(f"--{boundary}\r\n".encode("utf-8"))
        body.extend(f'Content-Disposition: form-data; name="message_thread_id"\r\n\r\n{thread_id}\r\n'.encode("utf-8"))

    # parse_mode
    body.extend(f"--{boundary}\r\n".encode("utf-8"))
    body.extend(b'Content-Disposition: form-data; name="parse_mode"\r\n\r\nHTML\r\n')

    # caption
    if caption_html:
        body.extend(f"--{boundary}\r\n".encode("utf-8"))
        body.extend(f'Content-Disposition: form-data; name="caption"\r\n\r\n{caption_html}\r\n'.encode("utf-8"))

    # document file
    mime_type = mimetypes.guess_type(file_path)[0] or "application/vnd.android.package-archive"
    body.extend(f"--{boundary}\r\n".encode("utf-8"))
    body.extend(
        f'Content-Disposition: form-data; name="document"; filename="{filename}"\r\n'
        f"Content-Type: {mime_type}\r\n\r\n".encode("utf-8")
    )

    with open(file_path, "rb") as f:
        body.extend(f.read())
    body.extend(f"\r\n--{boundary}--\r\n".encode("utf-8"))

    req = urllib.request.Request(
        url,
        data=body,
        headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
    )
    try:
        with urllib.request.urlopen(req, timeout=180) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except Exception as e:
        print(f"[telegram] sendDocument error for {filename}: {e}", file=sys.stderr)
        return None


def main():
    token, chat_id, thread_id = get_credentials()
    status = os.environ.get("BUILD_STATUS", "success").lower()
    branch = os.environ.get("BRANCH", "unknown")
    commit_sha = os.environ.get("COMMIT_SHA", "")
    short_sha = commit_sha[:7] if commit_sha else "unknown"
    commit_msg = os.environ.get("COMMIT_MSG", "").strip()
    first_line_msg = commit_msg.splitlines()[0] if commit_msg else ""
    first_line_esc = html.escape(first_line_msg)
    repo = os.environ.get("REPOSITORY", "HyperHLE/S5LWinBox")
    run_id = os.environ.get("RUN_ID", "")
    run_number = os.environ.get("RUN_NUMBER", "")
    actor = os.environ.get("ACTOR", "unknown")

    run_url = f"https://github.com/{repo}/actions/runs/{run_id}" if run_id else f"https://github.com/{repo}/actions"
    commit_url = f"https://github.com/{repo}/commit/{commit_sha}" if commit_sha else f"https://github.com/{repo}"

    if status == "success":
        text = (
            "✅ <b>Компиляция S5LWinBox успешно завершена!</b>\n\n"
            f"🌿 <b>Ветка:</b> <code>{html.escape(branch)}</code>\n"
            f'📌 <b>Коммит:</b> <a href="{commit_url}">{short_sha}</a> — <i>{first_line_esc}</i>\n'
            f"👤 <b>Автор:</b> {html.escape(actor)}\n"
            f'🔗 <b>Сборка:</b> <a href="{run_url}">GitHub Actions #{html.escape(str(run_number))}</a>\n\n'
            "📦 <b>Собранный архив:</b>\n"
            "• <b>S5LWinBox Windows x64:</b> <code>s5lwinbox.exe</code>, <code>SDL2.dll</code>, <code>hyperhle_gles.dll</code>\n\n"
            "<i>Отправляю архив...</i>"
        )
        send_telegram_message(token, chat_id, thread_id, text)

        import glob
        zip_files = glob.glob("dist/*.zip") + glob.glob("dist/*.7z")
        for zf in zip_files:
            if os.path.isfile(zf):
                size_mb = os.path.getsize(zf) / (1024 * 1024)
                zname = os.path.basename(zf)
                caption = (
                    f"🖥️ <b>{html.escape(zname)}</b> ({size_mb:.1f} MB)\n"
                    f"<b>Тип:</b> Windows x64 Portable (SDL2 + HyperHLE GLES)\n"
                    f"🌿 <b>Ветка:</b> <code>{html.escape(branch)}</code>\n"
                    f'📌 <b>Коммит:</b> <a href="{commit_url}">{short_sha}</a>'
                )
                print(f"[telegram] Sending {zf} ({size_mb:.1f} MB)...")
                send_telegram_document(token, chat_id, thread_id, zf, caption)
    else:
        text = (
            "❌ <b>Ошибка при сборке S5LWinBox!</b>\n\n"
            f"🌿 <b>Ветка:</b> <code>{html.escape(branch)}</code>\n"
            f'📌 <b>Коммит:</b> <a href="{commit_url}">{short_sha}</a> — <i>{first_line_esc}</i>\n'
            f"👤 <b>Автор:</b> {html.escape(actor)}\n"
            f'🔗 <b>Логи ошибки:</b> <a href="{run_url}">GitHub Actions #{html.escape(str(run_number))}</a>'
        )
        send_telegram_message(token, chat_id, thread_id, text)


if __name__ == "__main__":
    main()
