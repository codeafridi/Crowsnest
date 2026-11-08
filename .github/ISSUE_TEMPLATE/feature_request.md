---
name: Feature request
about: Suggest an idea for this project
title: ''
labels: ''
assignees: ''

---

---
name: Feature request
about: Suggest a feature
title: ''
labels: ['enhancement']
assignees: ''
---
**What do you want, in one sentence?**

Not the implementation but the outcome. "Show me which container is eating the most network traffic" beats "implement a Prometheus network exporter and parse conntrack".

**What do you do today to work around it?**

If your answer is "same as before, but mildly annoying", that's fine to say — it helps me judge how much this matters. If you're doing this by hand with another tool, name the tool and the command.

**Which tool already does this well?**

Crowsnest is deliberately modelled on shipped tools (htop, atop, node_exporter, the Datadog agent, Netdata and so on). If you know a tool that already solves this, name it and the version — it shapes how I build it, and it saves me the archaeology. Screenshots of how it presents the numbers are welcome.

**Anything it must not do?**

Crowsnest is local-only: it reads the machine it runs on and sends nothing anywhere unless you point an output at your own endpoint (webhook, `/metrics` scraper). If your idea only makes sense with telemetry or a phone-home, say which part would need it and I'll tell you whether there's a local answer.

**Machine info**

- `crowsnest --version`
- OS + desktop environment
