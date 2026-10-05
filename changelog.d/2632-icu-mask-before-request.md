### Fixed — ICU error mask is written before the line is requested, and survives suspend (#2632)

Kernel patch 0011 now masks the non-GPT ICU error sources before `devm_request_irq()`, so a source asserted at probe no longer storms the shared line (`irq 14: nobody cared`), and saves/restores the mask across suspend/resume. Bench-gated.
