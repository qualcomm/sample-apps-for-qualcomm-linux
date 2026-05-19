# ---------------------------------------------------------------------
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------

import streamlit as st

def _read_device_id() -> str:
    """
    Reads the Ubuntu device machine identifier (if available).
    On many embedded Linux targets this file exists; on others it may not.
    """
    path = "/sys/devices/soc0/machine"
    try:
        with open(path, "r", encoding="utf-8") as f:
            value = f.read().strip()
        return value or "Unknown"
    except Exception:
        return "Unknown"

st.set_page_config(page_title="Main Page", layout="wide")
st.title("GenAI Studio")

_device_id = _read_device_id()
st.markdown(
    """
    <style>
      .device-id-badge {
        position: fixed;
        top: 3.75rem;
        right: 1rem;
        z-index: 1000;
        padding: 0.25rem 0.5rem;
        border-radius: 0.5rem;
        font-size: 0.875rem;
        line-height: 1.2;
        max-width: 45vw;
        overflow: hidden;
        text-overflow: ellipsis;
        white-space: nowrap;
      }
    </style>
    """,
    unsafe_allow_html=True,
)
st.markdown(
    f'<div class="device-id-badge">Device: {_device_id}</div>',
    unsafe_allow_html=True,
)


col1, col2, col3, col4 = st.columns(4)
with col1:
    st.image("static/assets/ASR.png", use_container_width=True)
    if st.button("\U0001F399 Automatic speech recognition (ASR)"):
        st.switch_page("pages/ASR.py")
with col2:
    st.image("static/assets/Text2Image.png", use_container_width=True)
    if st.button("\U0001F3A8 Text2Image"):
        st.switch_page("pages/Text2Image.py")
with col3:
    st.image("static/assets/Text2Text.png",  use_container_width=True)
    if st.button("\U0001F4DD Text2Text"):
        st.switch_page("pages/Text2Text.py")
with col4:
    st.image("static/assets/Text2Speech.png",  use_container_width=True)
    if st.button("\U0001F4DD Text2Speech"):
        st.switch_page("pages/Text2Speech.py")