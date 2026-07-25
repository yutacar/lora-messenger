# Model layer

Phase 1 provides `Identity`, immutable validated `PostPayload`, and the bounded
in-memory `Timeline`. Wire-equivalent post data is separate from local receive order
and delivery state. These models remain independent of LVGL, Linux, storage, and any
radio implementation. Settings and persistence are deferred to Phase 3.
