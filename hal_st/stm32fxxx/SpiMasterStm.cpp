#include "hal_st/stm32fxxx/SpiMasterStm.hpp"
#include "generated/stm32fxxx/PeripheralTable.hpp"
#include "infra/event/EventDispatcher.hpp"
#include "infra/util/BitLogic.hpp"

namespace hal
{
    SpiMasterStm::SpiMasterStm(uint8_t oneBasedSpiIndex, GpioPinStm& clock, GpioPinStm& miso, GpioPinStm& mosi, const Config& config, GpioPinStm& slaveSelect)
        : spiInstance(oneBasedSpiIndex - 1)
        , clock(clock, PinConfigTypeStm::spiClock, oneBasedSpiIndex)
        , miso(miso, PinConfigTypeStm::spiMiso, oneBasedSpiIndex)
        , mosi(mosi, PinConfigTypeStm::spiMosi, oneBasedSpiIndex)
        , slaveSelect(slaveSelect, PinConfigTypeStm::spiSlaveSelect, oneBasedSpiIndex)
    {
        EnableClockSpi(spiInstance);

        SPI_HandleTypeDef spiHandle;
        spiHandle.Instance = peripheralSpi[spiInstance];
        spiHandle.Init.Mode = SPI_MODE_MASTER;
        spiHandle.Init.Direction = SPI_DIRECTION_2LINES;
        spiHandle.Init.DataSize = SPI_DATASIZE_8BIT;
        spiHandle.Init.CLKPolarity = config.polarityLow ? SPI_POLARITY_LOW : SPI_POLARITY_HIGH;
        spiHandle.Init.CLKPhase = config.phase1st ? SPI_PHASE_1EDGE : SPI_PHASE_2EDGE;
        spiHandle.Init.NSS = SPI_NSS_SOFT;
        spiHandle.Init.BaudRatePrescaler = config.baudRatePrescaler;
        spiHandle.Init.FirstBit = config.msbFirst ? SPI_FIRSTBIT_MSB : SPI_FIRSTBIT_LSB;
        spiHandle.Init.TIMode = SPI_TIMODE_DISABLED;
        spiHandle.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLED;
        spiHandle.Init.CRCPolynomial = 1;
#if defined(STM32F0) || defined(STM32F1) || defined(STM32F3) || defined(STM32F7) || defined(STM32WB)
        spiHandle.Init.CRCLength = SPI_CRC_LENGTH_8BIT;
        spiHandle.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
#endif
        HAL_SPI_Init(&spiHandle);

#if defined(STM32F0) || defined(STM32F1) || defined(STM32F3) || defined(STM32F7)
        peripheralSpi[spiInstance]->CR2 |= SPI_RXFIFO_THRESHOLD_QF;
#endif

        __HAL_SPI_ENABLE(&spiHandle);
    }

    void SpiMasterStm::SendAndReceive(infra::ConstByteRange sendData, infra::ByteRange receiveData, SpiAction nextAction, const infra::Function<void()>& onDone)
    {
        this->onDone = onDone;
        if (chipSelectConfigurator && !continuedSession)
            chipSelectConfigurator->StartSession();
        continuedSession = nextAction == SpiAction::continueSession;

        assert(sendData.size() == receiveData.size() || sendData.empty() || receiveData.empty());
        this->sendData = sendData;
        this->receiveData = receiveData;
        sending = !sendData.empty();
        receiving = !receiveData.empty();

        if (!sending)
            dummyToSend = receiveData.size();
        if (!receiving)
            dummyToReceive = sendData.size();

        really_assert(!spiInterruptRegistration);
        spiInterruptRegistration.Emplace(peripheralSpiIrq[spiInstance], [this]()
            { HandleInterrupt(); });

        peripheralSpi[spiInstance]->CR2 |= SPI_IT_TXE;
        peripheralSpi[spiInstance]->CR2 |= SPI_IT_RXNE;
    }

    void SpiMasterStm::SetChipSelectConfigurator(ChipSelectConfigurator& configurator)
    {
        chipSelectConfigurator = &configurator;
    }

    void SpiMasterStm::SetCommunicationConfigurator(CommunicationConfigurator& configurator)
    {
        communicationConfigurator = &configurator;
        communicationConfigurator->ActivateConfiguration();
    }

    void SpiMasterStm::ResetCommunicationConfigurator()
    {
        if (communicationConfigurator)
            communicationConfigurator->DeactivateConfiguration();

        communicationConfigurator = nullptr;
    }

    void SpiMasterStm::HandleInterrupt()
    {
        uint32_t status = peripheralSpi[spiInstance]->SR;
        if ((status & SPI_FLAG_RXNE) != 0)
        {
            if (dummyToReceive != 0)
            {
                (void)peripheralSpi[spiInstance]->DR;
                --dummyToReceive;
            }
            else if (receiving)
            {
                receiveData.front() = peripheralSpi[spiInstance]->DR;
                receiveData.pop_front();
            }

            receiving &= !receiveData.empty();

            if (dummyToReceive == 0 && !receiving)
                peripheralSpi[spiInstance]->CR2 &= ~SPI_IT_RXNE;
        }

        if ((status & SPI_FLAG_TXE) != 0)
        {
            if (dummyToSend != 0)
            {
                reinterpret_cast<volatile uint8_t&>(peripheralSpi[spiInstance]->DR) = 0;
                --dummyToSend;
            }
            else if (sending)
            {
                reinterpret_cast<volatile uint8_t&>(peripheralSpi[spiInstance]->DR) = sendData.front();
                sendData.pop_front();
            }

            sending &= !sendData.empty();

            // After the first transmit, disable interrupt on transmit buffer empty,
            // so that a receive is done before each transmit
            peripheralSpi[spiInstance]->CR2 &= ~SPI_IT_TXE;
        }

        really_assert(!(peripheralSpi[spiInstance]->SR & SPI_FLAG_OVR));

        spiInterruptRegistration->ClearPending();

        if (!sending && !receiving && dummyToSend == 0 && dummyToReceive == 0)
        {
            spiInterruptRegistration = infra::none;
            if (chipSelectConfigurator && !continuedSession)
                chipSelectConfigurator->EndSession();
            infra::EventDispatcher::Instance().Schedule([this]()
                { onDone(); });
        }
    }
}
