#!/usr/bin/env python3
"""
Simple webhook forwarder for Telegram/WhatsApp alerts
"""
import json
from fastapi import APIRouter, Request

router = APIRouter()

@router.post("/telegram")
async def telegram_webhook(request: Request):
    data = await request.json()
    return {"ok": True, "received": data}

@router.post("/whatsapp")
async def whatsapp_webhook(request: Request):
    data = await request.json()
    return {"ok": True, "received": data}
