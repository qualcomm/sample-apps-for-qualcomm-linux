# ---------------------------------------------------------------------
# Copyright (c) Qualcomm Innovation Center, Inc. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
# ---------------------------------------------------------------------

import streamlit as st
import requests

# Set page config
st.set_page_config(page_title="GenAI Studio")

# Page-specific key prefix
PAGE_KEY = "text2image"

# Sidebar layout
with st.sidebar:
    st.title('GenAI Studio')
    st.subheader('Solution')
    st.markdown('More info [doc](https://docs.qualcomm.com/bundle/publicresource/topics/80-70018-115/qualcomm-linux-docs-home.html?vproduct=1601111740013072&version=1.4)!')
    st.markdown('© Qualcomm Technologies, Inc.')
    st.markdown('---')

    st.title("Generation Settings")
    seed = st.number_input("Seed", min_value=0, max_value=999999, value=42, step=1)
    steps = st.slider("Steps", min_value=1, max_value=100, value=20)

    if st.button("🧹 Clear Chat History"):
        st.session_state[f"{PAGE_KEY}_messages"] = [{
            "role": "assistant",
            "content": "Hey there! 😄 How can I assist you today?"
        }]
        st.rerun()

# Server URL
server_url = 'http://0.0.0.0:8082/generate'

# Initialize page-specific chat history
if f"{PAGE_KEY}_messages" not in st.session_state:
    st.session_state[f"{PAGE_KEY}_messages"] = [{
        "role": "assistant",
        "content": "Hey there! 😄 How can I assist you today?"
    }]

# Display chat history
st.subheader("Chat History")
for message in st.session_state[f"{PAGE_KEY}_messages"]:
    with st.chat_message(message["role"]):
        st.write(f"{message['content']}")
    st.markdown("---")

# Chat input
if prompt := st.chat_input("Type your message..."):
    st.session_state[f"{PAGE_KEY}_messages"].append({
        "role": "user",
        "content": prompt
    })

    with st.chat_message("user"):
        st.write(prompt)

    with st.chat_message("assistant"):
        with st.spinner("Generating image..."):
            try:
                response = requests.post(
                    server_url,
                    json={"prompt": prompt, "seed": seed, "steps": steps}
                )
                if response.status_code == 200:
                    st.image(response.content, caption="Generated Image")
                    answer = "Here's the image generated based on your prompt."
                else:
                    answer = f"Error: {response.status_code} {response.text}"
            except Exception as e:
                answer = f"Request failed: {e}"

            st.write(answer)
            st.session_state[f"{PAGE_KEY}_messages"].append({
                "role": "assistant",
                "content": answer
            })
