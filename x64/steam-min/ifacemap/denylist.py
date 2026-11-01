"""Credential-destructive names and evidence; shared by validation and generation."""
import re


def denied_text(value):
    text = re.sub(r'[^a-z0-9]', '', str(value).lower())
    return any(word in text for word in
               ('credential', 'revoke', 'invalidate', 'logoff', 'clearloginusers'))


def denied(slot):
    return denied_text(slot['name']) or denied_text(slot.get('evidence', ''))
